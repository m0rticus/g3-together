// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcwire_bones.h"

#include <algorithm>
#include <utility>

namespace g3t::npcwire::bones {
namespace {

constexpr uint8_t DictionaryKind = 1;
constexpr uint8_t AckKind = 2;

void Put16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void Put32(std::vector<uint8_t>& out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void Put64(std::vector<uint8_t>& out, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

uint16_t Get16(uint8_t const* data)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) |
                                 (static_cast<uint16_t>(data[1]) << 8));
}

uint32_t Get32(uint8_t const* data)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[i]) << (i * 8);
    return value;
}

uint64_t Get64(uint8_t const* data)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (i * 8);
    return value;
}

void PutHeader(std::vector<uint8_t>& out, uint8_t kind, uint32_t world,
               uint64_t generation, uint16_t layoutId)
{
    out.push_back(Version);
    out.push_back(kind);
    Put16(out, 0);
    Put32(out, world);
    Put64(out, generation);
    Put16(out, layoutId);
}

Status ReadHeader(uint8_t const* data, std::size_t size, uint8_t kind,
                  uint32_t world, uint64_t generation, uint16_t& layoutId)
{
    if (!world || !generation) return Status::NotInitialized;
    if (!data || size < HeaderBytes || data[0] != Version || data[1] != kind ||
        Get16(data + 2) != 0 || !Get32(data + 4) || !Get64(data + 8) || !Get16(data + 16))
        return Status::Malformed;
    if (Get32(data + 4) != world || Get64(data + 8) != generation)
        return Status::WrongGeneration;
    layoutId = Get16(data + 16);
    return Status::Ok;
}

bool ValidCount(std::size_t count)
{
    return count != 0 && count <= static_cast<std::size_t>(pose::MaxBones);
}

bool ValidHashes(uint32_t const* hashes, std::size_t count)
{
    if (!hashes || !ValidCount(count)) return false;
    uint32_t sorted[pose::MaxBones];
    std::copy(hashes, hashes + count, sorted);
    std::sort(sorted, sorted + count);
    return sorted[0] && std::adjacent_find(sorted, sorted + count) == sorted + count;
}

bool ReadCount(uint8_t const* data, std::size_t size, std::size_t stride, std::size_t& count)
{
    if (size < HeaderBytes + CountBytes) return false;
    count = Get16(data + HeaderBytes);
    return ValidCount(count) && size == HeaderBytes + CountBytes + count * stride;
}

}

Status Dictionary::Reset(uint32_t world, uint64_t generation)
{
    if (!world || !generation) return Status::InvalidArgument;
    if (world != world_ || generation != generation_) {
        layouts_.clear();
        world_ = world;
        generation_ = generation;
    }
    return Status::Ok;
}

void Dictionary::Clear()
{
    layouts_.clear();
    world_ = 0;
    generation_ = 0;
}

Status Dictionary::Register(uint16_t layoutId, uint32_t const* hashes, std::size_t count)
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!layoutId || !ValidHashes(hashes, count)) return Status::InvalidArgument;
    const auto existing = layouts_.find(layoutId);
    if (existing != layouts_.end()) {
        const auto& layout = existing->second;
        return layout.size() == count && std::equal(layout.begin(), layout.end(), hashes)
            ? Status::Ok : Status::Conflict;
    }
    layouts_.emplace(layoutId, std::vector<uint32_t>(hashes, hashes + count));
    return Status::Ok;
}

Status Dictionary::FindLayout(uint32_t const* hashes, std::size_t count, uint16_t& layoutId) const
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!ValidHashes(hashes, count)) return Status::InvalidArgument;
    for (const auto& layout : layouts_) {
        if (layout.second.size() == count &&
            std::equal(layout.second.begin(), layout.second.end(), hashes)) {
            layoutId = layout.first;
            return Status::Ok;
        }
    }
    return Status::UnknownLayout;
}

bool Dictionary::HasLayout(uint16_t layoutId) const
{
    return layouts_.find(layoutId) != layouts_.end();
}

Status Dictionary::EncodeDictionary(uint16_t layoutId, std::vector<uint8_t>& message) const
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!layoutId) return Status::InvalidArgument;
    const auto layout = layouts_.find(layoutId);
    if (layout == layouts_.end()) return Status::UnknownLayout;
    std::vector<uint8_t> result;
    result.reserve(HeaderBytes + CountBytes + layout->second.size() * sizeof(uint32_t));
    PutHeader(result, DictionaryKind, world_, generation_, layoutId);
    Put16(result, static_cast<uint16_t>(layout->second.size()));
    for (const auto hash : layout->second) Put32(result, hash);
    message = std::move(result);
    return Status::Ok;
}

Status Dictionary::DecodeDictionary(uint8_t const* data, std::size_t size)
{
    uint16_t layoutId = 0;
    const auto status = ReadHeader(data, size, DictionaryKind, world_, generation_, layoutId);
    if (status != Status::Ok) return status;
    std::size_t count = 0;
    if (!ReadCount(data, size, sizeof(uint32_t), count)) return Status::Malformed;
    std::vector<uint32_t> hashes;
    hashes.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        hashes.push_back(Get32(data + HeaderBytes + CountBytes + i * sizeof(uint32_t)));
    if (!ValidHashes(hashes.data(), hashes.size())) return Status::Malformed;
    return Register(layoutId, hashes.data(), hashes.size());
}

Status Dictionary::EncodeAck(uint16_t layoutId, std::vector<uint8_t>& message) const
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!layoutId) return Status::InvalidArgument;
    if (!HasLayout(layoutId)) return Status::UnknownLayout;
    std::vector<uint8_t> result;
    result.reserve(HeaderBytes);
    PutHeader(result, AckKind, world_, generation_, layoutId);
    message = std::move(result);
    return Status::Ok;
}

Status Dictionary::DecodeAck(uint8_t const* data, std::size_t size, uint16_t& layoutId) const
{
    uint16_t incomingId = 0;
    const auto status = ReadHeader(data, size, AckKind, world_, generation_, incomingId);
    if (status != Status::Ok) return status;
    if (size != HeaderBytes) return Status::Malformed;
    if (!HasLayout(incomingId)) return Status::UnknownLayout;
    layoutId = incomingId;
    return Status::Ok;
}

Status EncodeRequest(uint32_t epoch, uint16_t layoutId, std::vector<uint8_t>& message)
{
    if (!epoch || !layoutId) return Status::InvalidArgument;
    std::vector<uint8_t> result;
    result.reserve(sizeof(Request));
    Put32(result, epoch);
    Put16(result, layoutId);
    message = std::move(result);
    return Status::Ok;
}

Status Dictionary::RespondToRequest(uint8_t const* data, std::size_t size,
                                    std::vector<uint8_t>& message) const
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!data || size != sizeof(Request) || !Get32(data) || !Get16(data + 4)) return Status::Malformed;
    if (Get32(data) != world_) return Status::WrongGeneration;
    return EncodeDictionary(Get16(data + 4), message);
}

Status Dictionary::EncodePose(uint16_t layoutId, pose::Bone const* bones, std::size_t count,
                              std::vector<uint8_t>& message) const
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!layoutId || !bones || !ValidCount(count)) return Status::InvalidArgument;
    const auto layout = layouts_.find(layoutId);
    if (layout == layouts_.end()) return Status::UnknownLayout;
    if (layout->second.size() != count) return Status::Conflict;
    for (std::size_t i = 0; i < count; ++i)
        if (bones[i].name != layout->second[i]) return Status::Conflict;
    std::vector<uint8_t> result;
    result.reserve(sizeof(PoseHeader) + count * TransformBytes);
    Put16(result, layoutId);
    for (std::size_t i = 0; i < count; ++i)
        for (const auto value : bones[i].values) Put16(result, static_cast<uint16_t>(value));
    message = std::move(result);
    return Status::Ok;
}

Status Dictionary::DecodePose(uint8_t const* data, std::size_t size, std::vector<pose::Bone>& bones,
                              uint16_t& layoutId) const
{
    if (!data || size < sizeof(PoseHeader) + TransformBytes || !Get16(data) ||
        (size - sizeof(PoseHeader)) % TransformBytes != 0) return Status::Malformed;
    if (!ValidCount((size - sizeof(PoseHeader)) / TransformBytes)) return Status::Malformed;
    const uint16_t incomingId = Get16(data);
    const auto layout = layouts_.find(incomingId);
    if (layout == layouts_.end()) {
        layoutId = incomingId;
        return Status::UnknownLayout;
    }
    const std::size_t count = layout->second.size();
    if (size != sizeof(PoseHeader) + count * TransformBytes) return Status::Malformed;
    std::vector<pose::Bone> result(count);
    for (std::size_t i = 0; i < count; ++i) {
        result[i].name = layout->second[i];
        for (std::size_t j = 0; j < 12; ++j) {
            const auto encoded = Get16(data + sizeof(PoseHeader) + i * TransformBytes + j * 2);
            const int32_t value = encoded < 0x8000u ? encoded : static_cast<int32_t>(encoded) - 0x10000;
            result[i].values[j] = static_cast<int16_t>(value);
        }
    }
    bones = std::move(result);
    layoutId = incomingId;
    return Status::Ok;
}

}
