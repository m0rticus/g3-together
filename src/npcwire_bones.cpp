// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcwire_bones.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace g3t::npcwire::bones {
namespace {

constexpr uint8_t DictionaryKind = 1;
constexpr uint8_t AckKind = 2;

inline void Write16(uint8_t* dst, uint16_t value) noexcept
{
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
}

inline void Write32(uint8_t* dst, uint32_t value) noexcept
{
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
    dst[2] = static_cast<uint8_t>(value >> 16);
    dst[3] = static_cast<uint8_t>(value >> 24);
}

inline void Write64(uint8_t* dst, uint64_t value) noexcept
{
    for (unsigned i = 0; i < 8; ++i) dst[i] = static_cast<uint8_t>(value >> (i * 8));
}

inline uint16_t Get16(uint8_t const* data) noexcept
{
    return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) |
                                 (static_cast<uint16_t>(data[1]) << 8));
}

inline uint32_t Get32(uint8_t const* data) noexcept
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

inline uint64_t Get64(uint8_t const* data) noexcept
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (i * 8);
    return value;
}

void WriteHeader(uint8_t* dst, uint8_t kind, uint32_t world,
                 uint64_t generation, uint16_t layoutId) noexcept
{
    dst[0] = Version;
    dst[1] = kind;
    Write16(dst + 2, 0);
    Write32(dst + 4, world);
    Write64(dst + 8, generation);
    Write16(dst + 16, layoutId);
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

bool ValidCount(std::size_t count) noexcept
{
    return count != 0 && count <= static_cast<std::size_t>(pose::MaxBones);
}

bool ValidHashes(uint32_t const* hashes, std::size_t count)
{
    if (!hashes || !ValidCount(count)) return false;
    constexpr unsigned Slots = static_cast<unsigned>(pose::MaxBones) * 2u;
    static_assert((Slots & (Slots - 1u)) == 0, "power-of-two slot count");
    uint32_t seen[Slots] = {};
    for (std::size_t i = 0; i < count; ++i) {
        const uint32_t hash = hashes[i];
        if (!hash) return false;
        unsigned slot = (hash * 2654435761u) & (Slots - 1u);
        while (seen[slot] && seen[slot] != hash) slot = (slot + 1u) & (Slots - 1u);
        if (seen[slot]) return false;
        seen[slot] = hash;
    }
    return true;
}

bool ValidBoneHashes(pose::Bone const* bones, std::size_t count)
{
    if (!bones || !ValidCount(count)) return false;
    constexpr unsigned Slots = static_cast<unsigned>(pose::MaxBones) * 2u;
    uint32_t seen[Slots] = {};
    for (std::size_t i = 0; i < count; ++i) {
        const uint32_t hash = bones[i].name;
        if (!hash) return false;
        unsigned slot = (hash * 2654435761u) & (Slots - 1u);
        while (seen[slot] && seen[slot] != hash) slot = (slot + 1u) & (Slots - 1u);
        if (seen[slot]) return false;
        seen[slot] = hash;
    }
    return true;
}

uint64_t DigestHashes(uint32_t const* hashes, std::size_t count) noexcept
{
    uint64_t digest = 14695981039346656037ull ^ static_cast<uint64_t>(count);
    for (std::size_t i = 0; i < count; ++i) {
        digest ^= static_cast<uint64_t>(hashes[i]);
        digest *= 1099511628211ull;
    }
    return digest;
}

uint64_t DigestBones(pose::Bone const* bones, std::size_t count) noexcept
{
    uint64_t digest = 14695981039346656037ull ^ static_cast<uint64_t>(count);
    for (std::size_t i = 0; i < count; ++i) {
        digest ^= static_cast<uint64_t>(bones[i].name);
        digest *= 1099511628211ull;
    }
    return digest;
}

bool ReadCount(uint8_t const* data, std::size_t size, std::size_t stride, std::size_t& count)
{
    if (size < HeaderBytes + CountBytes) return false;
    count = Get16(data + HeaderBytes);
    return ValidCount(count) && size == HeaderBytes + CountBytes + count * stride;
}

}

Dictionary::Dictionary(Dictionary const& other)
    : world_(other.world_), generation_(other.generation_), layouts_(other.layouts_), byDigest_(other.byDigest_)
{
    RebuildFastLayouts();
}

Dictionary& Dictionary::operator=(Dictionary const& other)
{
    if (this == &other) return *this;
    world_ = other.world_;
    generation_ = other.generation_;
    layouts_ = other.layouts_;
    byDigest_ = other.byDigest_;
    RebuildFastLayouts();
    return *this;
}

Dictionary::Dictionary(Dictionary&& other) noexcept
    : world_(other.world_),
      generation_(other.generation_),
      layouts_(std::move(other.layouts_)),
      fastLayouts_(std::move(other.fastLayouts_)),
      byDigest_(std::move(other.byDigest_))
{
    other.world_ = 0;
    other.generation_ = 0;
    other.fastLayouts_.clear();
}

Dictionary& Dictionary::operator=(Dictionary&& other) noexcept
{
    if (this == &other) return *this;
    world_ = other.world_;
    generation_ = other.generation_;
    layouts_ = std::move(other.layouts_);
    fastLayouts_ = std::move(other.fastLayouts_);
    byDigest_ = std::move(other.byDigest_);
    other.world_ = 0;
    other.generation_ = 0;
    other.fastLayouts_.clear();
    return *this;
}

Dictionary::LayoutEntry const* Dictionary::FindEntry(uint16_t layoutId) const noexcept
{
    if (layoutId < fastLayouts_.size()) return fastLayouts_[layoutId];
    const auto it = layouts_.find(layoutId);
    return it == layouts_.end() ? nullptr : &it->second;
}

void Dictionary::RebuildFastLayouts()
{
    fastLayouts_.clear();
    for (auto const& [id, entry] : layouts_) {
        if (id < FastLayoutLimit) {
            if (fastLayouts_.size() <= id) fastLayouts_.resize(static_cast<std::size_t>(id) + 1, nullptr);
            fastLayouts_[id] = &entry;
        }
    }
}

Status Dictionary::Reset(uint32_t world, uint64_t generation)
{
    if (!world || !generation) return Status::InvalidArgument;
    if (world != world_ || generation != generation_) {
        layouts_.clear();
        fastLayouts_.clear();
        byDigest_.clear();
        world_ = world;
        generation_ = generation;
    }
    return Status::Ok;
}

void Dictionary::Clear()
{
    layouts_.clear();
    fastLayouts_.clear();
    byDigest_.clear();
    world_ = 0;
    generation_ = 0;
}

Status Dictionary::Register(uint16_t layoutId, uint32_t const* hashes, std::size_t count)
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!layoutId || !ValidHashes(hashes, count)) return Status::InvalidArgument;
    if (const auto* existing = FindEntry(layoutId)) {
        const auto& layout = existing->hashes;
        return layout.size() == count && std::equal(layout.begin(), layout.end(), hashes)
            ? Status::Ok : Status::Conflict;
    }
    LayoutEntry entry;
    entry.hashes.assign(hashes, hashes + count);
    entry.identity.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        entry.identity[i].name = hashes[i];
        entry.identity[i].values[0] = entry.identity[i].values[4] = entry.identity[i].values[8] = pose::BasisScale == 4096.0f ? 4096 : static_cast<int16_t>(pose::BasisScale);
    }
    auto [it, inserted] = layouts_.emplace(layoutId, std::move(entry));
    if (inserted && layoutId < FastLayoutLimit) {
        if (fastLayouts_.size() <= layoutId) fastLayouts_.resize(static_cast<std::size_t>(layoutId) + 1, nullptr);
        fastLayouts_[layoutId] = &it->second;
    }
    auto& ids = byDigest_[DigestHashes(hashes, count)];
    ids.insert(std::lower_bound(ids.begin(), ids.end(), layoutId), layoutId);
    return Status::Ok;
}

Status Dictionary::FindLayout(uint32_t const* hashes, std::size_t count, uint16_t& layoutId) const
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!hashes || !ValidCount(count)) return Status::InvalidArgument;
    const auto bucket = byDigest_.find(DigestHashes(hashes, count));
    if (bucket != byDigest_.end()) {
        for (uint16_t id : bucket->second) {
            const auto* entry = FindEntry(id);
            if (entry && entry->hashes.size() == count &&
                std::equal(entry->hashes.begin(), entry->hashes.end(), hashes)) {
                layoutId = id;
                return Status::Ok;
            }
        }
    }
    return ValidHashes(hashes, count) ? Status::UnknownLayout : Status::InvalidArgument;
}

Status Dictionary::FindLayout(pose::Bone const* bones, std::size_t count, uint16_t& layoutId) const
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!bones || !ValidCount(count)) return Status::InvalidArgument;
    const auto bucket = byDigest_.find(DigestBones(bones, count));
    if (bucket != byDigest_.end()) {
        for (uint16_t id : bucket->second) {
            const auto* entry = FindEntry(id);
            if (entry && entry->hashes.size() == count) {
                bool match = true;
                for (std::size_t i = 0; i < count; ++i) {
                    if (bones[i].name != entry->hashes[i]) { match = false; break; }
                }
                if (match) {
                    layoutId = id;
                    return Status::Ok;
                }
            }
        }
    }
    return ValidBoneHashes(bones, count) ? Status::UnknownLayout : Status::InvalidArgument;
}

bool Dictionary::HasLayout(uint16_t layoutId) const
{
    return FindEntry(layoutId) != nullptr;
}

bool Dictionary::MatchesLayout(uint16_t layoutId, pose::Bone const* bones, std::size_t count) const noexcept
{
    if (!world_ || !generation_ || !layoutId || !bones || !ValidCount(count)) return false;
    const auto* entry = FindEntry(layoutId);
    if (!entry || entry->hashes.size() != count) return false;
    uint32_t const* expected = entry->hashes.data();
    for (std::size_t i = 0; i < count; ++i)
        if (bones[i].name != expected[i]) return false;
    return true;
}

Status Dictionary::IdentityPose(uint16_t layoutId, std::vector<pose::Bone>& bones) const
{
    const auto* entry = FindEntry(layoutId);
    if (!entry) return Status::UnknownLayout;
    bones = entry->identity;
    return Status::Ok;
}

std::vector<pose::Bone> const* Dictionary::IdentityPosePtr(uint16_t layoutId) const noexcept
{
    const auto* entry = FindEntry(layoutId);
    return entry ? &entry->identity : nullptr;
}

Status Dictionary::EncodeDictionary(uint16_t layoutId, std::vector<uint8_t>& message) const
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!layoutId) return Status::InvalidArgument;
    const auto* entry = FindEntry(layoutId);
    if (!entry) return Status::UnknownLayout;
    const std::size_t count = entry->hashes.size();
    std::vector<uint8_t> result(HeaderBytes + CountBytes + count * sizeof(uint32_t));
    WriteHeader(result.data(), DictionaryKind, world_, generation_, layoutId);
    Write16(result.data() + HeaderBytes, static_cast<uint16_t>(count));
    uint8_t* dst = result.data() + HeaderBytes + CountBytes;
    for (const auto hash : entry->hashes) {
        Write32(dst, hash);
        dst += 4;
    }
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
    uint32_t hashes[pose::MaxBones];
    uint8_t const* src = data + HeaderBytes + CountBytes;
    for (std::size_t i = 0; i < count; ++i) hashes[i] = Get32(src + i * sizeof(uint32_t));
    if (!ValidHashes(hashes, count)) return Status::Malformed;
    return Register(layoutId, hashes, count);
}

Status Dictionary::EncodeAck(uint16_t layoutId, std::vector<uint8_t>& message) const
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!layoutId) return Status::InvalidArgument;
    if (!HasLayout(layoutId)) return Status::UnknownLayout;
    std::vector<uint8_t> result(HeaderBytes);
    WriteHeader(result.data(), AckKind, world_, generation_, layoutId);
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
    std::vector<uint8_t> result(sizeof(Request));
    Write32(result.data(), epoch);
    Write16(result.data() + 4, layoutId);
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

Status Dictionary::ValidatePose(uint16_t layoutId, pose::Bone const* bones, std::size_t count) const
{
    if (!world_ || !generation_) return Status::NotInitialized;
    if (!layoutId || !bones || !ValidCount(count)) return Status::InvalidArgument;
    const auto* entry = FindEntry(layoutId);
    if (!entry) return Status::UnknownLayout;
    if (entry->hashes.size() != count) return Status::Conflict;
    uint32_t const* expected = entry->hashes.data();
    for (std::size_t i = 0; i < count; ++i)
        if (bones[i].name != expected[i]) return Status::Conflict;
    return Status::Ok;
}

Status Dictionary::EncodePose(uint16_t layoutId, pose::Bone const* bones, std::size_t count,
                              std::vector<uint8_t>& message) const
{
    const Status status = ValidatePose(layoutId, bones, count);
    if (status != Status::Ok) return status;
    std::vector<uint8_t> result(sizeof(PoseHeader) + count * TransformBytes);
    Write16(result.data(), layoutId);
    uint8_t* dst = result.data() + sizeof(PoseHeader);
    for (std::size_t i = 0; i < count; ++i) {
        for (const auto value : bones[i].values) {
            Write16(dst, static_cast<uint16_t>(value));
            dst += 2;
        }
    }
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
    const auto* entry = FindEntry(incomingId);
    if (!entry) {
        layoutId = incomingId;
        return Status::UnknownLayout;
    }
    const std::size_t count = entry->hashes.size();
    if (size != sizeof(PoseHeader) + count * TransformBytes) return Status::Malformed;
    std::vector<pose::Bone> result(count);
    uint8_t const* src = data + sizeof(PoseHeader);
    uint32_t const* expected = entry->hashes.data();
    for (std::size_t i = 0; i < count; ++i) {
        result[i].name = expected[i];
        for (std::size_t j = 0; j < 12; ++j) {
            result[i].values[j] = static_cast<int16_t>(Get16(src));
            src += 2;
        }
    }
    bones = std::move(result);
    layoutId = incomingId;
    return Status::Ok;
}

}
