// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pose.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace g3t::npcwire::bones {

// All messages are little-endian. Dictionary and ack use the generation-fenced
// header below (kind 1 and 2 respectively). A dictionary appends count:u16 and
// count ordered u32 hashes; an ack has no payload. A request is only epoch:u32,
// layout_id:u16 and may be sent without a known generation. A compact pose is
// layout_id:u16 followed by count * 12 i16 values; count comes from the layout.
// The live protocol 77 pose envelope additionally uses npcwire_pose.h's guarded
// acknowledged-reference/bitmask codec; this raw dictionary codec is its fallback.
// Counts are 1..pose::MaxBones. These structs describe bytes, not native loads.
constexpr uint8_t Version = 74;
#pragma pack(push, 1)
struct DefinitionHeader {
    uint8_t version, kind;
    uint16_t reserved;
    uint32_t epoch;
    uint64_t generation;
    uint16_t layoutId;
};
struct Request {
    uint32_t epoch;
    uint16_t layoutId;
};
struct PoseHeader {
    uint16_t layoutId;
};
#pragma pack(pop)
static_assert(sizeof(DefinitionHeader) == 18, "Bone definition header layout changed");
static_assert(sizeof(Request) == 6, "Bone request layout changed");
static_assert(sizeof(PoseHeader) == 2, "Compact pose header layout changed");
static_assert(offsetof(DefinitionHeader, generation) == 8 && offsetof(DefinitionHeader, layoutId) == 16,
              "Bone definition field offsets changed");
static_assert(offsetof(Request, layoutId) == 4 && offsetof(PoseHeader, layoutId) == 0,
              "Bone request or pose field offsets changed");
constexpr std::size_t HeaderBytes = sizeof(DefinitionHeader);
constexpr std::size_t CountBytes = 2;
constexpr std::size_t TransformBytes = 24;

enum class Status {
    Ok,
    InvalidArgument,
    NotInitialized,
    Malformed,
    WrongGeneration,
    UnknownLayout,
    UnknownPose,
    Conflict,
};

class Dictionary {
public:
    // Both tokens must be nonzero. A changed pair clears the cache; an identical
    // pair is a no-op. Use a fresh pair whenever old packets could still arrive.
    Status Reset(uint32_t world, uint64_t generation);
    void Clear();

    // Layout IDs are caller-assigned, nonzero, and must not be reused across
    // generations while older packets can arrive. Hashes must be nonzero and
    // unique within the ordered layout. Identical repeats are idempotent.
    Status Register(uint16_t layoutId, uint32_t const* hashes, std::size_t count);
    // Different IDs may describe the same layout; FindLayout returns the lowest.
    Status FindLayout(uint32_t const* hashes, std::size_t count, uint16_t& layoutId) const;
    bool HasLayout(uint16_t layoutId) const;
    Status IdentityPose(uint16_t layoutId, std::vector<pose::Bone>& bones) const;

    // Transmit dictionary messages and requests reliably. No method installs a
    // generation from a packet. Call Reset explicitly when the world changes.
    Status EncodeDictionary(uint16_t layoutId, std::vector<uint8_t>& message) const;
    Status DecodeDictionary(uint8_t const* data, std::size_t size);
    Status EncodeAck(uint16_t layoutId, std::vector<uint8_t>& message) const;
    Status DecodeAck(uint8_t const* data, std::size_t size, uint16_t& layoutId) const;
    Status RespondToRequest(uint8_t const* data, std::size_t size, std::vector<uint8_t>& message) const;

    // Pose values are exactly the already-quantized 12 int16s, without floats or
    // another quantization. Encode checks names and order against the dictionary.
    // DecodePose sets layoutId on Ok or UnknownLayout, including when no
    // dictionary is installed, allowing a request after a structurally valid
    // but early pose; bones changes only on Ok. The caller must fence epoch.
    // All other output arguments change only on Ok.
    Status EncodePose(uint16_t layoutId, pose::Bone const* bones, std::size_t count,
                      std::vector<uint8_t>& message) const;
    Status DecodePose(uint8_t const* data, std::size_t size, std::vector<pose::Bone>& bones,
                      uint16_t& layoutId) const;

private:
    uint32_t world_ = 0;
    uint64_t generation_ = 0;
    std::map<uint16_t, std::vector<uint32_t>> layouts_;
};

// Requests carry no generation: a host can answer after a same-epoch reset.
Status EncodeRequest(uint32_t epoch, uint16_t layoutId, std::vector<uint8_t>& message);

}
