// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-independent codec for the live NPC protocol.
#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <variant>
#include <vector>

namespace g3t::npcwire {

constexpr std::size_t MaxBatchBytes = 1172;
constexpr std::size_t MotionSlots = 8;
constexpr std::size_t MaxMetadataBytes = 49; // 3-byte mask + 46 bytes of values.
constexpr std::size_t MaxMotionOverrideBytes = 505; // 1-byte slot mask + eight 63-byte field-mask payloads.
constexpr std::size_t MaxBaselinesPerNpc = 16;
constexpr std::size_t AnchorReorderWindow = 64;
using Guid = std::array<std::uint8_t, 16>;
struct Stream { std::uint32_t epoch = 0, generation = 0; };

// These types describe byte offsets only. Integers and IEEE-754 floats are
// serialized explicitly in little-endian order, never by copying these structs.
#pragma pack(push, 1)
struct WireBatchHeader {
    std::uint8_t magic[2], version, reserved;
    std::uint8_t epoch[4], generation[4], sequence[4], count[2];
};
struct WireControl {
    std::uint8_t id[2], anchor[4], baseline[2], delta[6], yaw[2], orientation;
    std::uint8_t sent[4], flags, contact, slots;
};
struct WireMotionTick { std::uint8_t packed[3]; };
struct WireIdEntry { std::uint8_t id[2], guid[16]; };
struct WireBaselinePrefix { std::uint8_t id[2], anchor[4], baseline[2]; };
struct WireSignal { std::uint8_t id[2], incarnation[4], anchor[4], baseline[2], reason; };
struct WireQuaternion { std::uint8_t components[16]; };
struct WireFullMotion {
    std::uint8_t name[4], instance[4], phase[4], speed[4], weight[4], intro[4], extro[4];
    std::uint8_t blend[4], options[28], active;
};
struct WireFullRecord {
    std::uint8_t guid[16], position[12], rotation[16], movementMode, flags, hitPoints[4], hitPointsMax[4];
    std::uint8_t motion[4], aniState, heldRight[4], heldLeft[4], sent[4];
    std::uint8_t partyPlayer, partyType, waiting, guidePlayer, aiMode, action, attackReason, damageType;
    std::uint8_t damageAmount[4], damageMultiplier[4], incarnation[4], hitPlayer, hitSequence[2];
    std::uint8_t contact, attitude, combatTarget[4];
    WireFullMotion motions[8];
};
#pragma pack(pop)
static_assert(sizeof(WireBatchHeader) == 18, "batch header layout");
static_assert(sizeof(WireControl) == 24, "control layout");
static_assert(sizeof(WireMotionTick) == 3, "motion tick layout");
static_assert(sizeof(WireIdEntry) == 18, "id entry layout");
static_assert(sizeof(WireBaselinePrefix) == 8, "baseline prefix layout");
static_assert(sizeof(WireSignal) == 13, "signal layout");
static_assert(sizeof(WireQuaternion) == 16, "quaternion layout");
static_assert(sizeof(WireFullMotion) == 61, "full motion layout");
static_assert(sizeof(WireFullRecord) == 588, "full NPC layout");
static_assert(offsetof(WireFullRecord, motions) == 100, "full motion offset");
static_assert(offsetof(WireControl, slots) == 23, "slot mask offset");
static_assert(sizeof(WireControl) + sizeof(WireQuaternion) + 12 + 8 * 8 +
              MaxMetadataBytes + MaxMotionOverrideBytes <= MaxBatchBytes,
              "maximum exact motion override record fits one batch");

struct MotionDescriptor {
    std::uint32_t name = 0, instance = 0;
    float speed = 1.0f, intro = 0.0f, extro = 0.0f;
    std::int32_t blend = 0;
    std::array<std::uint32_t, 7> options{};
};
struct Motion {
    MotionDescriptor descriptor{};
    float phase = 0.0f, weight = 0.0f;
    std::uint8_t active = 0;
};
struct Anchor {
    std::array<float, 3> position{}; // Full-precision game coordinates in centimetres.
    std::array<float, 4> rotation{}; // Full original quaternion, including tilt.
};
struct Quaternion { std::array<float, 4> components{{0.0f, 0.0f, 0.0f, 1.0f}}; };
struct Yaw { double radians = 0.0; };
struct Control {
    Guid guid{};
    std::uint32_t anchor = 0;
    std::uint16_t baseline = 0;
    std::array<double, 3> position{}; // Centimetres.
    // Explicit alternative: full quaternion is exact; yaw is modulo one turn.
    std::variant<Quaternion, Yaw> rotation{Quaternion{}};
    std::uint32_t sent = 0;
    std::uint8_t movementMode = 0, flags = 0;
    std::int32_t hitPoints = 0, hitPointsMax = 1;
    std::uint32_t motion = 0;
    std::uint8_t aniState = 0;
    std::uint32_t heldRight = 0, heldLeft = 0;
    std::uint8_t partyPlayer = 0, partyType = 0, waiting = 0, guidePlayer = 0;
    std::uint8_t aiMode = 0, action = 0, attackReason = 0, damageType = 0;
    std::int32_t damageAmount = 0;
    float damageMultiplier = 0.0f;
    std::uint32_t incarnation = 1;
    std::uint8_t hitPlayer = 0;
    std::uint16_t hitSequence = 0;
    std::uint8_t contact = 0, attitude = 0;
    std::uint32_t combatTarget = 0; // Exact net::NpcRecord hash, not a short ID.
    std::uint8_t slots = 0; // Current slots whose active bit 0 is set.
    std::array<Motion, MotionSlots> motions{};
};

// RFC 1982 ordering: the half-range case is deliberately unordered.
bool Newer(std::uint32_t value, std::uint32_t previous);
std::uint8_t ActiveSlots(const Control& value);
bool EncodeYaw(double radians, std::uint16_t& result);
double DecodeYaw(std::uint16_t value);

class Context {
public:
    // Must be called for each world/connection reset. Epoch must advance, or
    // generation must advance within the same epoch. Zero is reserved.
    bool Reset(std::uint32_t epoch, std::uint32_t generation);
    bool Matches(Stream stream) const;
    // The stream and incarnation must come from the reliable message, not
    // from current local state; this rejects delayed pre-reset/pre-life data.
    bool AddId(Stream stream, std::uint16_t id, const Guid& guid);
    bool FindId(const Guid& guid, std::uint16_t& id) const;
    bool FindGuid(std::uint16_t id, Guid& guid) const;

    // Baseline parsing may also advance life, never backwards.
    bool AdvanceLife(Stream stream, std::uint16_t id, std::uint32_t incarnation);
    bool GetAnchor(std::uint16_t id, std::uint32_t token, Anchor& anchor) const;
    std::size_t RetainedBaselines(std::uint16_t id) const;
    std::size_t RetainedAnchors(std::uint16_t id) const;
    // Snapshot must contain a full quaternion and binary32-exact positions.
    // The last 16 distinct installs are retained FIFO; duplicate retries do not
    // refresh their lifetime. Evicted tokens cannot be reused or reinstalled,
    // even by exact retransmission. Anchor IDs must increase without wrap;
    // unseen IDs up to 63 values behind the highest are accepted. Retained
    // anchors can be shared by multiple baselines. Token fences persist across
    // lives and clear only on Reset; reset the stream before IDs are exhausted.
    bool AddBaseline(Stream stream, std::uint16_t id, std::uint16_t token,
                     std::uint32_t anchor, const Control& snapshot);

private:
    struct LifeAnchor { std::uint32_t life; Anchor value; };
    struct LifeBaseline { std::uint32_t life, anchor; Control value; std::vector<std::uint8_t> bytes; };
    struct History {
        std::bitset<65536> usedBaselines{};
        std::deque<std::uint16_t> retained;
        std::uint32_t highestAnchor = 0;
        std::uint64_t usedAnchors = 0;
    };
    bool InstallBaseline(Stream stream, std::uint16_t id, std::uint16_t token,
                         std::uint32_t anchor, const Control& snapshot, bool advanceLife,
                         const std::uint8_t* originalBytes);
    void RetireOtherLives(std::uint16_t id, std::uint32_t incarnation);
    bool ready_ = false;
    std::uint32_t epoch_ = 0, generation_ = 0;
    std::map<std::uint16_t, Guid> ids_;
    std::map<Guid, std::uint16_t> guids_;
    std::map<std::uint16_t, std::uint32_t> lives_;
    std::map<std::pair<std::uint16_t, std::uint32_t>, LifeAnchor> anchors_;
    std::map<std::pair<std::uint16_t, std::uint16_t>, LifeBaseline> baselines_;
    std::map<std::uint16_t, History> history_;
    std::map<std::uint16_t, std::uint32_t> sequences_;

    friend bool EncodeRecord(const Context&, const Control&, std::vector<std::uint8_t>&, bool);
    friend bool DecodeRecord(const Context&, const std::uint8_t*, std::size_t, Control&);
    friend bool EncodeBatch(const Context&, std::uint32_t, const std::vector<Control>&,
                            std::vector<std::uint8_t>&, bool);
    friend bool DecodeBatch(Context&, const std::uint8_t*, std::size_t, std::vector<Control>&);
    friend bool EncodeFullFallback(const Context&, std::uint32_t, const Control&, std::vector<std::uint8_t>&);
    friend bool DecodeFullFallback(Context&, const std::uint8_t*, std::size_t, Control&);
    friend bool EncodeId(const Context&, std::uint32_t, std::uint16_t, std::vector<std::uint8_t>&);
    friend bool DecodeId(Context&, const std::uint8_t*, std::size_t, std::uint16_t&);
    friend bool EncodeBaseline(const Context&, std::uint32_t, std::uint16_t, std::uint16_t, std::vector<std::uint8_t>&);
    friend bool DecodeBaseline(Context&, const std::uint8_t*, std::size_t, struct BaselineRef&);
    friend bool EncodeSignal(const Context&, std::uint32_t, const struct Signal&, std::vector<std::uint8_t>&);
    friend bool DecodeSignal(const Context&, const std::uint8_t*, std::size_t, struct Signal&);
};

// The 24-byte WireControl is followed by the optional full quaternion, then
// three bytes per active slot (or eight exact bytes/slot). Orientation bit 0
// selects yaw (0) or full quaternion (1); bit 7 announces metadata; bit 5
// announces relative field-mask motion overrides. Bit 6 selects exact binary32
// positions (12 bytes after the quaternion) and exact phase/weight
// (8 bytes/slot), requires full quaternion and zero delta/yaw fields. Other
// bits are reserved.
// Without metadata this is the original fixed format. With metadata, append a
// 3-byte little-endian, nonzero 21-bit mask and the selected original-width
// little-endian values, in this order (bit 0 first):
// movementMode:u8, hitPoints:i32, hitPointsMax:i32, motion:u32, aniState:u8,
// heldRight:u32, heldLeft:u32, partyPlayer:u8, partyType:u8, waiting:u8,
// guidePlayer:u8, aiMode:u8, action:u8, attackReason:u8, damageType:u8,
// damageAmount:i32, damageMultiplier:f32, hitPlayer:u8, hitSequence:u16,
// attitude:u8, combatTarget:u32. Mask bits 21-23 are reserved. Overrides must
// differ from the named baseline (binary32 bit equality for damageMultiplier);
// an overridden multiplier must be finite. Omitted values always come from
// that immutable baseline, never a previous update.
// Motion overrides follow metadata and contain a nonzero 8-bit slot mask.
// Each set slot has a nonzero 16-bit little-endian field mask followed only
// by changed fields in ascending field order: name, instance, speed, intro,
// extro, blend, options[0..6], active, inactive phase, inactive weight.
// Normal active phase/weight ticks still precede metadata and are not repeated
// by the override field payload. Omitted slot state is the immutable baseline.
//
// RecordBytes validates the structural prefix of one record in `available`
// bytes without a Context, leaving length unchanged on failure. It accepts
// subsequent records/trailing bytes; DecodeRecord requires an exact size and
// additionally validates values and canonical overrides against the baseline.
bool RecordBytes(const std::uint8_t* bytes, std::size_t available, std::size_t& length);

// All outputs and Context ordering remain unchanged on failure. EncodeRecord
// fails on changes to GUID or incarnation, non-finite motion/active values,
// active phase outside [0,1], live native weight outside [0,10], or position
// overflow: send a new baseline or full fallback instead. Dynamic descriptors,
// active flags and inactive phase/weight use relative field-mask motion overrides.
// Slots
// contain phase rounded to 12 bits and weight rounded to 8 bits, with the top
// four bits zero. Standalone records lack sequence/stream fences: use
// DecodeBatch on the network.
bool EncodeRecord(const Context& context, const Control& value, std::vector<std::uint8_t>& output,
                  bool exact = false);
bool DecodeRecord(const Context& context, const std::uint8_t* bytes, std::size_t size, Control& output);
bool EncodeBatch(const Context& context, std::uint32_t sequence, const std::vector<Control>& values,
                 std::vector<std::uint8_t>& output, bool exact = false);
bool DecodeBatch(Context& context, const std::uint8_t* bytes, std::size_t size, std::vector<Control>& output);

// Lossless 588-byte legacy-field keyframe, in an 18-byte fenced envelope.
// Requires a Quaternion and positions exactly representable as binary32.
// All eight slots are included, regardless of `slots`; tokens are not sent.
// Decoding sets `slots` from the active bits in those slots.
// The ID and current life must already be installed via reliable control.
bool EncodeFullFallback(const Context& context, std::uint32_t sequence, const Control& value,
                        std::vector<std::uint8_t>& output);
bool DecodeFullFallback(Context& context, const std::uint8_t* bytes, std::size_t size, Control& output);

struct BaselineRef {
    std::uint16_t id = 0, baseline = 0;
    std::uint32_t incarnation = 0, anchor = 0;
};
enum class SignalKind : std::uint8_t { Ack, Resync };
enum class ResyncReason : std::uint8_t { None, UnknownId, UnknownBaseline, UnknownAnchor, Invalid };
struct Signal {
    SignalKind kind = SignalKind::Ack;
    BaselineRef reference{};
    ResyncReason reason = ResyncReason::None;
    std::uint32_t sequence = 0; // Message sequence, not a control-order update.
};
// Reliable messages use the same stream-fenced header. IDs and baselines are
// immutable and accept byte-exact retransmissions while retained. An evicted
// baseline and any control referencing it fail closed and need resync with
// fresh tokens. ACKs identify a particular baseline; caller manages readiness.
bool EncodeId(const Context&, std::uint32_t sequence, std::uint16_t id, std::vector<std::uint8_t>&);
bool DecodeId(Context&, const std::uint8_t*, std::size_t, std::uint16_t& id);
bool EncodeBaseline(const Context&, std::uint32_t sequence, std::uint16_t id, std::uint16_t token,
                    std::vector<std::uint8_t>&);
bool DecodeBaseline(Context&, const std::uint8_t*, std::size_t, BaselineRef&);
bool EncodeSignal(const Context&, std::uint32_t sequence, const Signal&, std::vector<std::uint8_t>&);
bool DecodeSignal(const Context&, const std::uint8_t*, std::size_t, Signal&);

} // namespace g3t::npcwire
