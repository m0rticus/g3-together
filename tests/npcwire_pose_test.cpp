// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcwire_pose.h"
#include "../src/npcsend_policy.h"
#include <cassert>
#include <cstdio>
#include <random>

using namespace g3t;
using namespace g3t::npcwire::bones;

static bool Same(std::vector<pose::Bone> const& a, std::vector<pose::Bone> const& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(pose::Bone)) == 0;
}

static Dictionary Layout(size_t count)
{
    Dictionary d;
    assert(d.Reset(1, 1) == Status::Ok);
    std::vector<uint32_t> names;
    for (size_t i = 0; i < count; ++i) names.push_back(static_cast<uint32_t>(i + 1));
    assert(d.Register(1, names.data(), names.size()) == Status::Ok);
    return d;
}

static void Codec()
{
    auto d = Layout(80);
    std::vector<pose::Bone> bones(80), decoded;
    std::mt19937 random(74);
    for (size_t i = 0; i < bones.size(); ++i) {
        bones[i].name = static_cast<uint32_t>(i + 1);
        for (auto& v : bones[i].values) v = static_cast<int16_t>(random());
    }
    PoseCache sender, receiver;
    std::vector<uint8_t> bytes;
    bool key = false;
    uint16_t id = 0;
    assert(sender.Encode(d, 1, 1, 0, bones.data(), bones.size(), 0, bytes, key) == Status::Ok && key);
    assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok && Same(bones, decoded));
    assert(bytes[DeltaHeaderBytes] == 0); // arbitrary affine values select a raw key
    for (size_t size = 0; size < bytes.size(); ++size) {
        const auto previous = decoded;
        assert(receiver.Decode(d, bytes.data(), size, 1, 0, decoded, id) != Status::Ok);
        assert(Same(previous, decoded));
    }
    assert(sender.Store(1, 1, 1, 0, bones.data(), bones.size(), 0, true));
    assert(receiver.Store(1, 1, 1, 0, bones.data(), bones.size(), 0, false));
    assert(sender.Ack(1, 1));
    // Quantization error introduced by this codec is identically zero for
    // every basis/translation integer, including shear and signed extrema.
    for (int k = 0; k < 12; ++k) {
        const int16_t previous = bones[0].values[k];
        for (int value = -32768; value <= 32767; ++value) {
            bones[0].values[k] = static_cast<int16_t>(value);
            assert(sender.Encode(d, 1, 1, 0, bones.data(), bones.size(), 1, bytes, key) == Status::Ok && !key);
            assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok && Same(bones, decoded));
            auto a = pose::Decode(bones[0]), b = pose::Decode(decoded[0]);
            for (int n = 0; n < 12; ++n) assert(a.values[n] == b.values[n]);
        }
        bones[0].values[k] = previous;
    }
    bones[0].values[0] ^= 0x7fff;
    assert(sender.Encode(d, 1, 1, 0, bones.data(), bones.size(), 2, bytes, key) == Status::Ok && !key);
    const auto valid = bytes;
    const auto unchanged = decoded;
    for (size_t size = 0; size < valid.size(); ++size) {
        decoded = unchanged;
        assert(receiver.Decode(d, valid.data(), size, 1, 0, decoded, id) != Status::Ok);
        assert(Same(decoded, unchanged));
    }
    bytes = valid; bytes.push_back(0);
    assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);
    bytes = valid; bytes[DeltaHeaderBytes + 1] = 18;
    assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);
    bytes = valid; bytes[DeltaHeaderBytes + 1 + 12 + 10] |= 0x80;
    assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);

    // A single residual can span the complete int16 range in either direction.
    for (int k = 0; k < 12; ++k) {
        bones[0].values[k] = -32768;
        bones[1].values[k] = 32767;
    }
    PoseCache extrema;
    assert(extrema.Store(1, 1, 1, 0, bones.data(), bones.size(), 0, true));
    assert(extrema.Ack(1, 1));
    for (int k = 0; k < 12; ++k) {
        bones[0].values[k] = 32767;
        bones[1].values[k] = -32768;
    }
    assert(extrema.Encode(d, 1, 1, 0, bones.data(), bones.size(), 1, bytes, key) == Status::Ok && !key);
    assert(extrema.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok && Same(bones, decoded));
}

static void Recovery()
{
    auto d = Layout(1);
    std::vector<pose::Bone> bones(1), decoded;
    bones[0].name = 1;
    PoseCache sender, receiver;
    std::vector<uint8_t> bytes;
    bool key;
    uint16_t id = 0;
    auto receive = [&](uint32_t token) {
        assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
        assert(Same(decoded, bones));
        assert(receiver.Store(token, 1, 1, 0, decoded.data(), 1, token, false));
    };
    assert(sender.Encode(d, 1, 1, 0, bones.data(), 1, 0, bytes, key) == Status::Ok && key);
    sender.Store(1, 1, 1, 0, bones.data(), 1, 0, key); receive(1);
    sender.Ack(1, 1);
    for (uint32_t token = 2; token < 60; ++token) {
        ++bones[0].values[9];
        assert(sender.Encode(d, 1, 1, 0, bones.data(), 1, token, bytes, key) == Status::Ok);
        assert(sender.Store(token, 1, 1, 0, bones.data(), 1, token, key));
        // Loss of a delta and its ACK does not affect successors.
        if (token % 5 == 0) continue;
        receive(token);
        if (token % 7 != 0) sender.Ack(token, 1);
        assert(sender.Retained() <= PoseWindow && receiver.Retained() <= PoseWindow);
    }
    assert(!sender.Ack(1, 1) && !sender.Ack(59, 2));
    assert(!receiver.Store(1, 1, 1, 0, bones.data(), 1, 60, false)); // reorder/duplicate
    assert(sender.Encode(d, 1, 1, 0, bones.data(), 1, PoseKeyMs, bytes, key) == Status::Ok && key);
    PoseCache rejoin;
    assert(rejoin.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
    sender.Store(60, 1, 1, 0, bones.data(), 1, PoseKeyMs, true); sender.Ack(60, 1);
    ++bones[0].values[9];
    assert(sender.Encode(d, 1, 1, 0, bones.data(), 1, PoseKeyMs + 1, bytes, key) == Status::Ok && !key);
    assert(rejoin.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::UnknownPose);
    assert(sender.Ack(0, 1));
    assert(sender.Encode(d, 1, 1, 0, bones.data(), 1, PoseKeyMs + 2, bytes, key) == Status::Ok && key);
    assert(rejoin.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
    assert(sender.Encode(d, 1, 2, pose::Facial, bones.data(), 1, PoseKeyMs + 3, bytes, key) == Status::Ok && key);
    sender.Store(61, 2, 1, pose::Facial, bones.data(), 1, PoseKeyMs + 3, key);
    assert(sender.Retained() == 1 && !sender.Ack(60, 1));
    assert(rejoin.Decode(d, bytes.data(), bytes.size(), 2, pose::Facial, decoded, id) == Status::Ok);
    rejoin.Store(61, 2, 1, pose::Facial, decoded.data(), 1, PoseKeyMs + 3, false);
    assert(rejoin.Decode(d, bytes.data(), bytes.size(), 1, pose::Facial, decoded, id) == Status::WrongGeneration);
    auto other = Layout(1);
    other.Clear();
    assert(rejoin.Decode(other, bytes.data(), bytes.size(), 2, pose::Facial, decoded, id) == Status::UnknownLayout);

    for (size_t count : {1u, 7u, 8u, 9u, 255u, 256u}) {
        auto layout = Layout(count);
        std::vector<pose::Bone> rig;
        layout.IdentityPose(1, rig);
        PoseCache source, sink;
        assert(source.Encode(layout, 1, 1, 0, rig.data(), count, 0, bytes, key) == Status::Ok && key);
        assert(sink.Decode(layout, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok && Same(decoded, rig));
        assert(source.Store(UINT32_MAX - 1, 1, 1, 0, rig.data(), count, 0, true));
        assert(sink.Store(UINT32_MAX - 1, 1, 1, 0, rig.data(), count, 0, false));
        source.Ack(UINT32_MAX - 1, 1);
        rig.back().values[11] = 32767;
        assert(source.Encode(layout, 1, 1, 0, rig.data(), count, 1, bytes, key) == Status::Ok);
        assert(sink.Decode(layout, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok && Same(decoded, rig));
        source.Store(UINT32_MAX, 1, 1, 0, rig.data(), count, 1, key);
        assert(source.Store(1, 1, 1, 0, rig.data(), count, 2, false)); // zero token skipped on wrap
        assert(source.Ack(1, 1));
    }
}

static void IndependentRiceKeys()
{
    auto dictionary = Layout(64);
    std::vector<pose::Bone> rig, decoded, reconstructed;
    assert(dictionary.IdentityPose(1, rig) == Status::Ok);
    for (size_t i = 0; i < rig.size(); ++i) {
        for (int k = 0; k < 9; ++k)
            rig[i].values[k] += static_cast<int16_t>((k + 1) * 29 + i / 8);
        for (int k = 9; k < 12; ++k)
            rig[i].values[k] = static_cast<int16_t>(1800 + (k - 9) * 200 + i * 7);
    }
    PoseCache sender, receiver;
    std::vector<uint8_t> bytes;
    bool key = false;
    uint16_t id = 0;
    assert(sender.EncodeCompact(dictionary, 1, 1, 0, rig.data(), rig.size(), 0,
                                bytes, key, reconstructed) == Status::Ok);
    assert(key && ReadPose32(bytes.data() + 2) == 0 && bytes[6] == 3);
    assert(ReadPose32(bytes.data() + 8) == 0 && bytes[12] == 0 && bytes[13] == 0);
    assert(bytes.size() < 500 && Same(rig, reconstructed));
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
    assert(Same(rig, decoded));
    const auto valid = bytes;
    const auto unchanged = decoded;
    for (size_t size = 0; size < valid.size(); ++size) {
        assert(receiver.Decode(dictionary, valid.data(), size, 1, 0, decoded, id) != Status::Ok);
        assert(Same(decoded, unchanged));
    }
    bytes = valid; bytes.push_back(0);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);
    bytes = valid; bytes[7] = 2;
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);
    bytes = valid; bytes[8] = 1;
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);
    bytes = valid; bytes[12] = 1;
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);

    // Explicit mode-2 key, including a zero-presence bone.
    auto small = Layout(2);
    std::vector<pose::Bone> expected;
    assert(small.IdentityPose(1, expected) == Status::Ok);
    expected[0].values[9] = -1;
    PoseRiceWriter writer;
    writer.Put(1, 16); writer.Put(0, 32); writer.Put(2, 8);
    writer.Put(0, 8); writer.Put(0, 32); writer.Put(0, 16);
    for (int k = 0; k < 12; ++k) writer.Put(0, 4);
    writer.Put(1, 1); writer.Put(0, 1); writer.Align();
    for (int k = 0; k < 12; ++k) writer.Rice(k == 9 ? 1 : 0, 0, 21);
    writer.Align();
    assert(receiver.Decode(small, writer.bytes.data(), writer.bytes.size(), 1, 0, decoded, id) == Status::Ok);
    assert(Same(decoded, expected));
    auto invalid = writer.bytes;
    invalid[20] |= 0x80; // nonzero padding in the presence bitmap
    assert(receiver.Decode(small, invalid.data(), invalid.size(), 1, 0, decoded, id) == Status::Malformed);

    // Spatial keys carry a full int16-range jump and then repeat it without a payload.
    assert(dictionary.IdentityPose(1, rig) == Status::Ok);
    rig[0].values[9] = -32768;
    for (size_t i = 1; i < rig.size(); ++i) rig[i].values[9] = 32767;
    assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), 0, bytes, key) == Status::Ok);
    assert(key && bytes[6] == 3);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
    assert(Same(rig, decoded));

    // Independent low-amplitude, uncorrelated residuals select temporal Rice.
    assert(dictionary.IdentityPose(1, rig) == Status::Ok);
    std::mt19937 lowAmplitude(123);
    for (auto& bone : rig)
        for (auto& value : bone.values)
            value += static_cast<int16_t>(int(lowAmplitude() % 7) - 3);
    assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), 0, bytes, key) == Status::Ok);
    assert(key && bytes[6] == 2);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
    assert(Same(rig, decoded));

    // Independent keys must also preserve arbitrary affine signed extrema.
    std::mt19937 random(987);
    for (int sample = 0; sample < 30; ++sample) {
        for (auto& bone : rig)
            for (auto& value : bone.values) value = static_cast<int16_t>(random());
        rig[0].values[0] = -32768;
        rig[1].values[0] = 32767;
        assert(sender.EncodeCompact(dictionary, 1, 1, 0, rig.data(), rig.size(), sample,
                                    bytes, key, reconstructed) == Status::Ok && key);
        assert(Same(rig, reconstructed));
        assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
        assert(Same(rig, decoded));
    }
}

static void ReliableAnchors()
{
    auto dictionary = Layout(64);
    std::vector<pose::Bone> rig, decoded, reconstructed, anchor;
    assert(dictionary.IdentityPose(1, rig) == Status::Ok);
    for (size_t i = 0; i < rig.size(); ++i) {
        rig[i].values[1] = static_cast<int16_t>(70 + i);
        rig[i].values[9] = static_cast<int16_t>(1000 + i * 3);
    }
    PoseCache sender, receiver, lateReceiver;
    std::vector<uint8_t> bytes;
    bool key = false;
    uint16_t id = 0;
    assert(sender.AnchorDue(0));
    assert(sender.EncodeCompact(dictionary, 1, 1, 0, rig.data(), rig.size(), 0,
                                bytes, key, reconstructed, nullptr, true) == Status::Ok && key);
    anchor = reconstructed;
    assert(sender.Store(1, 1, 1, 0, anchor.data(), anchor.size(), 0, true, true));
    assert(!sender.AnchorDue(1, 1, 1, 0));
    assert(sender.AnchorDue(1, 2, 1, 0));
    assert(sender.AnchorDue(1, 1, 2, 0));
    assert(sender.AnchorDue(1, 1, 1, pose::Facial));
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
    assert(receiver.Store(1, 1, 1, 0, decoded.data(), decoded.size(), 0, true, true));
    assert(!sender.AnchorDue(4999) && sender.AnchorDue(5000));

    // 2.5 seconds at 60 Hz: all unacknowledged snapshots are independent.
    for (uint32_t token = 2; token <= 151; ++token) {
        const uint32_t now = (token - 1) * 1000 / 60;
        ++rig[0].values[9];
        assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), now, bytes, key) == Status::Ok);
        assert(key && ReadPose32(bytes.data() + 2) == 0);
        assert(sender.Store(token, 1, 1, 0, rig.data(), rig.size(), now, key));
        assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
        assert(Same(rig, decoded));
        assert(receiver.Store(token, 1, 1, 0, decoded.data(), decoded.size(), now, false));
        assert(lateReceiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
        assert(lateReceiver.Store(token, 1, 1, 0, decoded.data(), decoded.size(), now, false));
        assert(sender.Retained() <= PoseWindow + PoseRetainedAnchors);
        assert(receiver.Retained() <= PoseWindow + PoseRetainedAnchors);
    }
    assert(!sender.Ack(2, 1));
    assert(sender.Ack(1, 1)); // retained well beyond the transient window
    assert(lateReceiver.Store(1, 1, 1, 0, anchor.data(), anchor.size(), 2500, true, true));
    assert(lateReceiver.Store(1, 1, 1, 0, anchor.data(), anchor.size(), 2501, true, true));
    auto conflict = anchor;
    ++conflict[0].values[0];
    assert(!lateReceiver.Store(1, 1, 1, 0, conflict.data(), conflict.size(), 2502, true, true));
    assert(!lateReceiver.Store(150, 1, 1, 0, rig.data(), rig.size(), 2502, false));

    // No additional ACKs, and both receivers retain the referenced key.
    for (uint32_t token = 152; token <= 260; ++token) {
        const uint32_t now = (token - 1) * 1000 / 60;
        ++rig[0].values[9];
        assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), now, bytes, key) == Status::Ok);
        assert(!key && ReadPose32(bytes.data() + 2) == 1);
        assert(sender.Store(token, 1, 1, 0, rig.data(), rig.size(), now, key));
        assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
        assert(Same(rig, decoded));
        assert(receiver.Store(token, 1, 1, 0, decoded.data(), decoded.size(), now, false));
        assert(lateReceiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
        assert(Same(rig, decoded));
        assert(lateReceiver.Store(token, 1, 1, 0, decoded.data(), decoded.size(), now, false));
        assert(sender.Retained() <= PoseWindow + PoseRetainedAnchors);
        assert(receiver.Retained() <= PoseWindow + PoseRetainedAnchors);
        assert(lateReceiver.Retained() <= PoseWindow + PoseRetainedAnchors);
    }
    assert(sender.Ack(1, 1)); // still retained after more than 250 samples
    assert(sender.AnchorDue(5000)); // transient independent keys do not postpone reliable anchors
    assert(sender.EncodeCompact(dictionary, 1, 1, 0, rig.data(), rig.size(), 5000,
                                bytes, key, reconstructed, nullptr, true) == Status::Ok && key);
    assert(ReadPose32(bytes.data() + 2) == 0 && Same(rig, reconstructed));
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
    assert(Same(rig, decoded));
    assert(sender.Store(261, 1, 1, 0, reconstructed.data(), reconstructed.size(), 5000, true, true));
    assert(!sender.AnchorDue(9999) && sender.AnchorDue(10000));
    assert(sender.Ack(261, 1));
    assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), 5001, bytes, key) == Status::Ok && !key);
    assert(ReadPose32(bytes.data() + 2) == 261);

    // The first plus four recent keys, including out-of-order installations, stay bounded.
    PoseCache bounded;
    assert(bounded.Store(10, 1, 1, 0, anchor.data(), anchor.size(), 10, true, true));
    assert(bounded.Store(30, 1, 1, 0, anchor.data(), anchor.size(), 30, true, true));
    assert(bounded.Store(20, 1, 1, 0, anchor.data(), anchor.size(), 100, true, true));
    assert(!bounded.AnchorDue(5029) && bounded.AnchorDue(5030));
    assert(bounded.Store(40, 1, 1, 0, anchor.data(), anchor.size(), 40, true, true));
    assert(bounded.Store(50, 1, 1, 0, anchor.data(), anchor.size(), 50, true, true));
    assert(bounded.Retained() == PoseRetainedAnchors && bounded.Ack(10, 1));
    assert(bounded.Store(10, 1, 1, 0, anchor.data(), anchor.size(), 60, true, true));
    assert(!bounded.Store(10, 1, 1, 0, conflict.data(), conflict.size(), 60, true, true));
    assert(bounded.Ack(20, 1) && bounded.Ack(50, 1));
    assert(!bounded.Store(49, 1, 1, 0, anchor.data(), anchor.size(), 60, false));
    assert(!bounded.Store(60, 1, 1, 0, anchor.data(), anchor.size(), 60, false, true));
    assert(bounded.Store(60, 1, 1, 0, anchor.data(), anchor.size(), 60, true));
    assert(!bounded.Store(60, 1, 1, 0, conflict.data(), conflict.size(), 61, true, true));
    assert(bounded.Store(60, 1, 1, 0, anchor.data(), anchor.size(), 61, true, true));
    assert(bounded.Retained() == PoseRetainedAnchors && bounded.Ack(60, 1));
    assert(!bounded.Ack(20, 1) && bounded.Ack(10, 1));

    // An older ACK remains usable after a newer transient ACK is evicted.
    PoseCache fallback;
    assert(fallback.Store(1, 1, 1, 0, anchor.data(), anchor.size(), 0, true, true));
    assert(fallback.Store(2, 1, 1, 0, anchor.data(), anchor.size(), 1, false));
    assert(fallback.Ack(2, 1) && fallback.Ack(1, 1));
    for (uint32_t token = 3; token <= 40; ++token)
        assert(fallback.Store(token, 1, 1, 0, anchor.data(), anchor.size(), token, false));
    assert(fallback.Encode(dictionary, 1, 1, 0, anchor.data(), anchor.size(), 41,
                           bytes, key) == Status::Ok && !key);
    assert(ReadPose32(bytes.data() + 2) == 1);

    // A changed layout, flags, or incarnation clears anchors and ACK references.
    assert(sender.Store(262, 1, 2, 0, rig.data(), rig.size(), 5002, true, true));
    assert(sender.Retained() == 1 && !sender.Ack(261, 1));
    assert(!sender.Store(261, 1, 1, 0, anchor.data(), anchor.size(), 5003, true, true));
    assert(sender.Store(263, 1, 2, pose::Facial, rig.data(), rig.size(), 5004, false));
    assert(sender.Retained() == 1 && !sender.Ack(262, 1) && sender.AnchorDue(5004));
    assert(sender.Store(1, 2, 1, 0, rig.data(), rig.size(), 5005, true, true));
    assert(sender.Retained() == 1 && !sender.Ack(263, 1) && !sender.Ack(0, 1));
    assert(!sender.Store(264, 1, 2, pose::Facial, rig.data(), rig.size(), 5006, true, true));
    assert(receiver.Store(1, 2, 1, 0, rig.data(), rig.size(), 5005, true, true));
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::WrongGeneration);
}

static void SecondOrderSpatial()
{
    auto dictionary = Layout(64);
    std::vector<pose::Bone> rig, decoded, reconstructed, identity;
    assert(dictionary.IdentityPose(1, identity) == Status::Ok);
    rig = identity;
    for (size_t i = 0; i < rig.size(); ++i)
        for (int k = 0; k < 12; ++k)
            rig[i].values[k] += static_cast<int16_t>(100 + k * 7 + i * (k + 3));
    PoseCache sender, receiver;
    std::vector<uint8_t> bytes;
    bool key = false;
    uint16_t layout = 0;
    assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), 0, bytes, key) == Status::Ok);
    assert(key && bytes[6] == 3 && bytes[7] == 1 && bytes.size() < 80);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
    assert(Same(rig, decoded));
    auto valid = bytes;
    const auto unchanged = decoded;
    for (size_t size = 0; size < valid.size(); ++size) {
        assert(receiver.Decode(dictionary, valid.data(), size, 1, 0, decoded, layout) != Status::Ok);
        assert(Same(decoded, unchanged));
    }
    bytes = valid; bytes.push_back(0);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Malformed);
    bytes = valid; bytes[7] = 2;
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Malformed);
    bytes = valid; bytes[6] = 2;
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Malformed);

    // Both spatial modes accept the full second-order int16 residual range,
    // but reject an escaped value just outside that range.
    auto extremeDictionary = Layout(3);
    std::vector<pose::Bone> base, extreme;
    assert(extremeDictionary.IdentityPose(1, base) == Status::Ok);
    base[0].values[9] = base[2].values[9] = 32767;
    base[1].values[9] = -32768;
    extreme = base;
    extreme[0].values[9] = extreme[2].values[9] = -32768;
    extreme[1].values[9] = 32767;
    PoseCache extremeReceiver;
    assert(extremeReceiver.Store(1, 1, 1, 0, base.data(), base.size(), 0, true, true));
    auto extremePayload = [](bool rigidMode, bool invalid) {
        PoseRiceWriter writer;
        writer.Put(1, 16); writer.Put(1, 32); writer.Put(rigidMode ? 4 : 3, 8);
        writer.Put(rigidMode ? 4 : 1, 8); writer.Put(0, 32); writer.Put(0, 16);
        for (int k = 0; k < 12; ++k) writer.Put(0, 4);
        for (int i = 0; i < 3; ++i) writer.Put(1, 1);
        writer.Align();
        if (rigidMode) {
            for (int i = 0; i < 3; ++i) writer.Put(0, 1);
            writer.Align();
        }
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 12; ++k)
                writer.Rice(k != 9 ? 0u : i == 0 ? 131069u : i == 1 ? 262140u :
                            invalid ? 524281u : 524279u, 0, 21);
        writer.Align();
        return writer.bytes;
    };
    for (bool rigidMode : {false, true}) {
        bytes = extremePayload(rigidMode, false);
        assert(extremeReceiver.Decode(extremeDictionary, bytes.data(), bytes.size(), 1, 0,
                                      decoded, layout) == Status::Ok && Same(extreme, decoded));
        bytes = extremePayload(rigidMode, true);
        assert(extremeReceiver.Decode(extremeDictionary, bytes.data(), bytes.size(), 1, 0,
                                      decoded, layout) == Status::Malformed);
        bytes = extremePayload(rigidMode, false);
        bytes[7] = 0; // the legacy first-order form has a lower residual bound
        assert(extremeReceiver.Decode(extremeDictionary, bytes.data(), bytes.size(), 1, 0,
                                      decoded, layout) == Status::Malformed);
    }
    PoseRiceWriter unusedSecond;
    unusedSecond.Put(1, 16); unusedSecond.Put(1, 32); unusedSecond.Put(4, 8);
    unusedSecond.Put(4, 8); unusedSecond.Put(0, 32); unusedSecond.Put(0, 16);
    for (int k = 0; k < 12; ++k) unusedSecond.Put(0, 4);
    for (int i = 0; i < 3; ++i) unusedSecond.Put(0, 1);
    unusedSecond.Align();
    for (int i = 0; i < 3; ++i) unusedSecond.Put(0, 1);
    unusedSecond.Align();
    assert(extremeReceiver.Decode(extremeDictionary, unusedSecond.bytes.data(), unusedSecond.bytes.size(),
                                  1, 0, decoded, layout) == Status::Malformed);

    // Missing affine bones interrupt compatibility; after a new run, the
    // second previous residual becomes usable again.
    auto transitions = Layout(7);
    std::vector<pose::Bone> transitionBase, expected;
    assert(transitions.IdentityPose(1, transitionBase) == Status::Ok);
    expected = transitionBase;
    for (int i = 0; i < 7; ++i) expected[i].values[9] = static_cast<int16_t>((i + 1) * 10);
    PoseCache transitionReceiver;
    assert(transitionReceiver.Store(1, 1, 1, 0, transitionBase.data(), transitionBase.size(), 0, true));
    PoseRiceWriter writer;
    writer.Put(1, 16); writer.Put(1, 32); writer.Put(4, 8); writer.Put(4, 8);
    writer.Put(0, 32); writer.Put(0, 16);
    for (int k = 0; k < 12; ++k) writer.Put(0, 4);
    for (int i = 0; i < 7; ++i) writer.Put(i != 2 && i != 5, 1);
    writer.Align();
    for (int i = 0; i < 7; ++i) writer.Put(0, 1);
    writer.Align();
    for (int i = 0; i < 7; ++i) {
        if (i == 2 || i == 5) continue;
        const unsigned delta = i == 3 ? 40 : i == 6 ? 70 : 10;
        for (int k = 0; k < 12; ++k) writer.Rice(k == 9 ? delta * 2 : 0, 0, 21);
    }
    writer.Align();
    assert(transitionReceiver.Decode(transitions, writer.bytes.data(), writer.bytes.size(), 1, 0,
                                     decoded, layout) == Status::Ok && Same(expected, decoded));
    bytes = writer.bytes; bytes[7] |= 8;
    assert(transitionReceiver.Decode(transitions, bytes.data(), bytes.size(), 1, 0,
                                     decoded, layout) == Status::Malformed);

    // Grouped coarse quaternions: changing the omitted index breaks the
    // spatial run, then first- and second-order prediction resume.
    auto quaternionDictionary = Layout(6);
    std::vector<pose::Bone> quaternionBase, quaternionExpected;
    assert(quaternionDictionary.IdentityPose(1, quaternionBase) == Status::Ok);
    quaternionExpected = quaternionBase;
    PoseCache quaternionReceiver;
    assert(quaternionReceiver.Store(1, 1, 1, 0, quaternionBase.data(), quaternionBase.size(), 0, true));
    PoseRiceWriter quaternionWriter;
    quaternionWriter.Put(1, 16); quaternionWriter.Put(1, 32); quaternionWriter.Put(4, 8);
    quaternionWriter.Put(7, 8); quaternionWriter.Put(0, 32); quaternionWriter.Put(0, 16);
    for (int k = 0; k < 12; ++k) quaternionWriter.Put(0, 4);
    for (int i = 0; i < 6; ++i) quaternionWriter.Put(1, 1);
    quaternionWriter.Align();
    for (int i = 0; i < 6; ++i) quaternionWriter.Put(1, 1);
    quaternionWriter.Align();
    for (int i = 0; i < 6; ++i) quaternionWriter.Put(i < 3 ? 3 : 0, 2);
    quaternionWriter.Align();
    for (int i = 0; i < 6; ++i) {
        const uint8_t omitted = i < 3 ? 3 : 0;
        const int component = i < 3 ? 0 : 3;
        PoseQuat quaternion{};
        quaternion.omitted = omitted;
        quaternion.values[component] = 4 * (i % 3 + 1);
        assert(PoseCompleteQuat(quaternion));
        pose::Bone matrix;
        assert(PoseQuatToBone(quaternion, quaternionBase[i], matrix));
        quaternionExpected[i] = matrix;
        quaternionExpected[i].values[9] = static_cast<int16_t>(i < 3 ? (i + 1) * 10 : 100 + (i - 3) * 10);
        const int angular = i == 3 ? -4095 : i % 3 == 2 ? 0 : 1;
        for (int k = 0; k < 4; ++k)
            if (k != omitted) quaternionWriter.Rice(k == component ? PoseZigZag(angular) : 0, 0, 21);
        const int translation = i == 3 ? 100 : i % 3 == 2 ? 0 : 10;
        for (int k = 9; k < 12; ++k)
            quaternionWriter.Rice(k == 9 ? PoseZigZag(translation) : 0, 0, 21);
    }
    quaternionWriter.Align();
    assert(quaternionReceiver.Decode(quaternionDictionary, quaternionWriter.bytes.data(),
                                     quaternionWriter.bytes.size(), 1, 0, decoded, layout) == Status::Ok);
    assert(Same(quaternionExpected, decoded));
    bytes = quaternionWriter.bytes; bytes[23] |= 0x80; // omitted-index padding
    assert(quaternionReceiver.Decode(quaternionDictionary, bytes.data(), bytes.size(), 1, 0,
                                     decoded, layout) == Status::Malformed);

    // Prefer temporal extrapolation when it is smaller; the older reference is
    // optional, so periodic motion can instead use the recent anchor alone.
    PoseCache temporalSender, temporalReceiver;
    auto recent = identity, target = identity;
    for (size_t i = 0; i < recent.size(); ++i)
        for (int k = 0; k < 12; ++k) {
            const int16_t change = static_cast<int16_t>(50 + i + k);
            recent[i].values[k] += change;
            target[i].values[k] += static_cast<int16_t>(2 * change);
        }
    assert(temporalSender.Store(1, 1, 1, 0, identity.data(), identity.size(), 100, true, true));
    assert(temporalSender.Store(2, 1, 1, 0, recent.data(), recent.size(), 200, true, true));
    assert(temporalReceiver.Store(1, 1, 1, 0, identity.data(), identity.size(), 100, true, true));
    assert(temporalReceiver.Store(2, 1, 1, 0, recent.data(), recent.size(), 200, true, true));
    assert(temporalSender.Ack(1, 1) && temporalSender.Ack(2, 1));
    assert(temporalSender.Encode(dictionary, 1, 1, 0, target.data(), target.size(), 300,
                                 bytes, key) == Status::Ok && !key);
    assert(bytes[6] == 2 && ReadPose32(bytes.data() + 2) == 2 && ReadPose32(bytes.data() + 8) == 1);
    assert(bytes[12] == 0 && bytes[13] == 1);
    assert(temporalReceiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
    assert(Same(target, decoded));

    PoseCache periodicSender, periodicReceiver;
    auto old = identity;
    target = identity;
    std::mt19937 variation(833);
    for (size_t i = 0; i < old.size(); ++i)
        for (int k = 0; k < 12; ++k) {
            old[i].values[k] += static_cast<int16_t>(int(variation() % 16001) - 8000);
            target[i].values[k] += static_cast<int16_t>(100 + k * 7 + i * (k + 3));
        }
    assert(periodicSender.Store(1, 1, 1, 0, old.data(), old.size(), 100, true, true));
    assert(periodicSender.Store(2, 1, 1, 0, identity.data(), identity.size(), 200, true, true));
    assert(periodicReceiver.Store(1, 1, 1, 0, old.data(), old.size(), 100, true, true));
    assert(periodicReceiver.Store(2, 1, 1, 0, identity.data(), identity.size(), 200, true, true));
    assert(periodicSender.Ack(1, 1) && periodicSender.Ack(2, 1));
    assert(periodicSender.Encode(dictionary, 1, 1, 0, target.data(), target.size(), 300,
                                 bytes, key) == Status::Ok && !key);
    assert(bytes[6] == 3 && bytes[7] == 1 && ReadPose32(bytes.data() + 2) == 2 &&
           ReadPose32(bytes.data() + 8) == 0 && bytes[12] == 0 && bytes[13] == 0);
    assert(periodicReceiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
    assert(Same(target, decoded));

    // Real rigid bones plus affine and different omitted-quaternion runs.
    assert(sender.Store(1, 1, 1, 0, identity.data(), identity.size(), 0, true, true));
    assert(receiver.Store(1, 1, 1, 0, identity.data(), identity.size(), 0, true, true));
    assert(sender.Ack(1, 1));
    for (size_t i = 0; i < rig.size(); ++i) {
        pose::Transform transform;
        transform.name = static_cast<uint32_t>(i + 1);
        double angle = .05 + double(i) * .006;
        if (i >= 32 && i < 36) {
            angle = 3.12 - double(i - 32) * .004;
            transform.values[0] = 1;
            transform.values[4] = transform.values[8] = float(std::cos(angle));
            transform.values[5] = float(-std::sin(angle));
            transform.values[7] = -transform.values[5];
        } else {
            transform.values[0] = transform.values[8] = float(std::cos(angle));
            transform.values[2] = float(std::sin(angle));
            transform.values[6] = -transform.values[2];
            transform.values[4] = 1;
        }
        if (i >= 16 && i < 21) transform.values[0] += .5f + float(i - 16) * .003f;
        transform.values[9] = float(i) * .5f;
        transform.values[10] = float(i) * -.25f;
        transform.values[11] = float(i) * .125f;
        assert(pose::Encode(transform, rig[i]));
    }
    PoseStats stats;
    assert(sender.EncodeCompact(dictionary, 1, 1, 0, rig.data(), rig.size(), 100,
                                bytes, key, reconstructed, &stats) == Status::Ok && !key);
    assert(bytes[6] == 4 && (bytes[7] & 4) && stats.rigidBones && stats.maxBasisErrorUnits <= 2);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
    assert(Same(decoded, reconstructed));
    for (size_t i = 0; i < rig.size(); ++i)
        for (int k = 0; k < 12; ++k)
            assert(std::abs(int(rig[i].values[k]) - int(decoded[i].values[k])) <= (k < 9 ? 2 : 0));
    valid = bytes;
    for (size_t size = 0; size < valid.size(); ++size)
        assert(receiver.Decode(dictionary, valid.data(), size, 1, 0, decoded, layout) != Status::Ok);
    bytes = valid; bytes[7] |= 8;
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Malformed);
    assert(receiver.Store(1, 2, 1, 0, identity.data(), identity.size(), 101, true, true));
    assert(receiver.Decode(dictionary, valid.data(), valid.size(), 1, 0, decoded, layout) == Status::WrongGeneration);

    // BUG-5: Mode 4 with secondOrder=true must reconstruct identically on encoder and decoder.
    PoseCache mode4SecondSender, mode4SecondReceiver;
    auto frameA = identity, frameB = identity, frameC = identity;
    for (size_t i = 0; i < frameA.size(); ++i) {
        auto makeFrame = [&](int step, pose::Bone& out) {
            pose::Transform tr;
            tr.name = static_cast<uint32_t>(i + 1);
            const double a = 0.04 * step + 0.015 * double(i);
            tr.values[0] = tr.values[8] = float(std::cos(a));
            tr.values[2] = float(std::sin(a));
            tr.values[6] = -tr.values[2];
            tr.values[4] = 1.0f;
            // Chaotic temporal velocity on bone 0 + rigid constant spatial offset across bones makes secondOrder win!
            const float jitter = (step == 2) ? 18.0f : (step == 1 ? -9.0f : 0.0f);
            tr.values[9] = jitter + float(i) * 0.25f;
            tr.values[10] = -jitter * 0.5f;
            tr.values[11] = jitter * 0.25f;
            assert(pose::Encode(tr, out));
        };
        makeFrame(0, frameA[i]);
        makeFrame(1, frameB[i]);
        makeFrame(3, frameC[i]);
    }
    assert(mode4SecondSender.Store(1, 1, 1, 0, frameA.data(), frameA.size(), 100, true, true));
    assert(mode4SecondSender.Store(2, 1, 1, 0, frameB.data(), frameB.size(), 200, true, true));
    assert(mode4SecondReceiver.Store(1, 1, 1, 0, frameA.data(), frameA.size(), 100, true, true));
    assert(mode4SecondReceiver.Store(2, 1, 1, 0, frameB.data(), frameB.size(), 200, true, true));
    assert(mode4SecondSender.Ack(1, 1) && mode4SecondSender.Ack(2, 1));
    assert(mode4SecondSender.EncodeCompact(dictionary, 1, 1, 0, frameC.data(), frameC.size(), 300,
                                           bytes, key, reconstructed, &stats) == Status::Ok && !key);
    assert(bytes[6] == 4);
    assert(mode4SecondReceiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
    assert(Same(decoded, reconstructed));
}

static void LongDelayedFirstAnchor()
{
    auto dictionary = Layout(64);
    std::vector<pose::Bone> rig, decoded;
    assert(dictionary.IdentityPose(1, rig) == Status::Ok);
    for (size_t i = 0; i < rig.size(); ++i) {
        rig[i].values[1] = static_cast<int16_t>(70 + i);
        rig[i].values[9] = static_cast<int16_t>(1000 + i * 3);
    }
    PoseCache sender, receiver;
    std::vector<uint8_t> bytes;
    uint32_t token = 1;
    uint16_t layout = 0;
    bool key = false;
    assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), 0,
                         bytes, key, nullptr, true) == Status::Ok && key);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
    assert(Same(rig, decoded));
    assert(sender.Store(token, 1, 1, 0, rig.data(), rig.size(), 0, true, true));
    assert(receiver.Store(token, 1, 1, 0, decoded.data(), decoded.size(), 0, true, true));
    const auto first = rig;

    // No ACK returns for 55 seconds; eleven later reliable keys and hundreds
    // of transient samples must not evict the original key at either end.
    for (uint32_t period = 1; period <= 11; ++period) {
        for (uint32_t step = 1; step <= 35; ++step) {
            const uint32_t now = (period - 1) * PoseKeyMs + step * 138;
            ++rig[0].values[9];
            assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), now,
                                 bytes, key) == Status::Ok && key);
            assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
            assert(Same(rig, decoded));
            ++token;
            assert(sender.Store(token, 1, 1, 0, rig.data(), rig.size(), now, key));
            assert(receiver.Store(token, 1, 1, 0, decoded.data(), decoded.size(), now, false));
        }
        const uint32_t now = period * PoseKeyMs;
        assert(sender.AnchorDue(now));
        assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), now,
                             bytes, key, nullptr, true) == Status::Ok && key);
        assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
        assert(Same(rig, decoded));
        ++token;
        assert(sender.Store(token, 1, 1, 0, rig.data(), rig.size(), now, true, true));
        assert(receiver.Store(token, 1, 1, 0, decoded.data(), decoded.size(), now, true, true));
        assert(sender.Retained() <= PoseWindow + PoseRetainedAnchors);
        assert(receiver.Retained() <= PoseWindow + PoseRetainedAnchors);
    }
    assert(sender.Retained() == PoseWindow + PoseRetainedAnchors);
    assert(receiver.Retained() == PoseWindow + PoseRetainedAnchors);
    assert(sender.Ack(1, 1));
    assert(receiver.Store(1, 1, 1, 0, first.data(), first.size(), 55000, true, true));
    auto conflict = first;
    ++conflict[0].values[0];
    assert(!receiver.Store(1, 1, 1, 0, conflict.data(), conflict.size(), 55000, true, true));

    ++rig[0].values[9];
    assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), 55001,
                         bytes, key) == Status::Ok && !key);
    assert(ReadPose32(bytes.data() + 2) == 1);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
    assert(Same(rig, decoded));
    const auto delayedDelta = bytes;
    const auto delayedPose = rig;
    ++token;
    assert(sender.Store(token, 1, 1, 0, rig.data(), rig.size(), 55001, false));
    assert(receiver.Store(token, 1, 1, 0, decoded.data(), decoded.size(), 55001, false));

    // Keep the first anchor after its ACK too: an in-flight delta may still use it.
    for (uint32_t period = 12; period <= 16; ++period) {
        const uint32_t now = period * PoseKeyMs;
        ++rig[0].values[9];
        assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), now,
                             bytes, key, nullptr, true) == Status::Ok && key);
        assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
        assert(Same(rig, decoded));
        ++token;
        assert(sender.Store(token, 1, 1, 0, rig.data(), rig.size(), now, true, true));
        assert(receiver.Store(token, 1, 1, 0, decoded.data(), decoded.size(), now, true, true));
        assert(sender.Retained() <= PoseWindow + PoseRetainedAnchors);
        assert(receiver.Retained() <= PoseWindow + PoseRetainedAnchors);
    }
    assert(receiver.Decode(dictionary, delayedDelta.data(), delayedDelta.size(), 1, 0,
                           decoded, layout) == Status::Ok && Same(delayedPose, decoded));
    assert(sender.Encode(dictionary, 1, 1, 0, rig.data(), rig.size(), 80001,
                         bytes, key) == Status::Ok && !key);
    assert(ReadPose32(bytes.data() + 2) == 1);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
    assert(Same(rig, decoded));
    auto layoutReset = sender;
    assert(layoutReset.Store(token + 1, 1, 2, 0, rig.data(), rig.size(), 80002, false));
    assert(layoutReset.Retained() == 1 && !layoutReset.Ack(1, 1));
    assert(!layoutReset.Store(1, 1, 1, 0, first.data(), first.size(), 80002, true, true));
    auto flagsReset = sender;
    assert(flagsReset.Store(token + 1, 1, 1, pose::Facial, rig.data(), rig.size(), 80002, false));
    assert(flagsReset.Retained() == 1 && !flagsReset.Ack(1, 1));
    assert(!flagsReset.Store(1, 1, 1, 0, first.data(), first.size(), 80002, true, true));
    assert(sender.Store(1, 2, 1, 0, rig.data(), rig.size(), 80002, true, true));
    assert(receiver.Store(1, 2, 1, 0, rig.data(), rig.size(), 80002, true, true));
    assert(!sender.Ack(1, 1));
    assert(receiver.Decode(dictionary, delayedDelta.data(), delayedDelta.size(), 1, 0,
                           decoded, layout) == Status::WrongGeneration);
}

static void Simulation()
{
    constexpr int count = 75, seconds = 20, hz = 60;
    auto dictionary = Layout(80);
    struct Npc {
        net::NpcRecord record{};
        npcsend::State policy;
        PoseCache sender, receiver;
        std::vector<pose::Bone> bones = std::vector<pose::Bone>(80);
        uint32_t token = 0;
        std::deque<std::pair<uint32_t, uint32_t>> acks;
    };
    std::vector<Npc> npcs(count);
    uint64_t before = 0, rateOnly = 0, unbatched = 0, after = 0, sends = 0, keys = 0, deltas = 0, byTier[4] = {};
    uint64_t boneBytes = 0, logical = 0, advertised[3] = {};
    auto envelope = [](size_t payload) {
        return payload + ((payload + 1171) / 1172) * 100; // conservative 4+24 ENet + 72 relay
    };
    for (int tick = 0; tick < seconds * hz; ++tick) {
        const uint32_t now = static_cast<uint32_t>(tick * 1000 / hz);
        size_t datagram = 4;
        auto command = [&](size_t payload, size_t header) {
            if (datagram + header + payload > 1200) {
                after += datagram + 72;
                datagram = 4;
            }
            datagram += header + payload;
        };
        for (int i = 0; i < count; ++i) {
            auto& n = npcs[i];
            while (!n.acks.empty() && n.acks.front().first <= now) {
                n.sender.Ack(n.acks.front().second, 1);
                n.acks.pop_front();
            }
            // 5 fighting, 35 walking, 15 breathing/idling, 15 static, 5 far walking.
            const bool fighting = i < 5, walking = (i >= 5 && i < 40) || i >= 70, idle = i >= 40 && i < 55;
            n.record.guid[0] = static_cast<uint8_t>(i + 1); n.record.rotation[3] = 1;
            n.record.sent = now;
            if (walking) n.record.position[0] = tick * 2.0f;
            if (walking || fighting || idle) {
                n.record.motions[0].name = n.record.motions[0].instance = 1;
                n.record.motions[0].active = 1; n.record.motions[0].weight = 1;
                n.record.motions[0].phase = float(tick % 120) / 120;
            }
            for (size_t b = 0; b < n.bones.size(); ++b) {
                const double t = tick / 60.0, offset = b * 0.17;
                const double amplitude = fighting ? .4 : walking ? .18 : idle ? .012 : 0;
                const double angle = amplitude * std::sin(t * (fighting ? 6 : 3) + offset);
                pose::Transform bone; bone.name = static_cast<uint32_t>(b + 1);
                bone.values[0] = bone.values[8] = static_cast<float>(std::cos(angle));
                bone.values[2] = static_cast<float>(std::sin(angle)); bone.values[6] = -bone.values[2];
                bone.values[4] = 1;
                for (int k = 9; k < 12; ++k)
                    bone.values[k] = static_cast<float>(b * .5 + amplitude * 10 * std::sin(t * 3 + offset + k));
                assert(pose::Encode(bone, n.bones[b]));
            }
            // Baseline calibrated to the observed 2,702 poses/s class; no live logs in tests.
            if (tick % 5 < 3) before += envelope(23 + 18 + 24 + 3 + 3 + 80 * 24);
            npcsend::Input in;
            in.record = &n.record; in.bones = n.bones.data(); in.boneCount = 80;
            in.now = now; in.moving = walking; in.combat = fighting; in.distanceCm = i >= 40 ? 4000 : 1500;
            const auto result = npcsend::Evaluate(n.policy, in);
            if (result.decision == npcsend::Decision::Wait) continue;
            assert(npcsend::Commit(n.policy, in, result.decision));
            ++sends; ++byTier[static_cast<unsigned>(result.tier)];
            const uint32_t hint = npcsend::PresentationInterval(n.policy, in);
            ++advertised[hint <= 16 ? 0 : hint < 100 ? 1 : 2];
            std::vector<uint8_t> bytes;
            bool key = false;
            assert(n.sender.Encode(dictionary, 1, 1, 0, n.bones.data(), 80, now, bytes, key) == Status::Ok);
            const uint32_t token = ++n.token;
            assert(n.sender.Store(token, 1, 1, 0, n.bones.data(), 80, now, key));
            std::vector<pose::Bone> decoded;
            uint16_t id;
            // 5% snapshot loss, then 5% ACK loss: no chained deltas.
            if (token % 20 != 0) {
                assert(n.receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
                assert(Same(decoded, n.bones));
                n.receiver.Store(token, 1, 1, 0, decoded.data(), 80, now, false);
                if (token % 19 != 0) n.acks.emplace_back(now + 17, token);
            }
            const size_t control = 23 + 2 + 18 + 24 + 3 + 2;
            rateOnly += envelope(control + 2 + 80 * 24);
            const size_t payload = control + bytes.size();
            unbatched += envelope(payload);
            if (payload <= 1172) command(payload, 8);
            else for (size_t offset = 0; offset < payload; offset += 1172)
                command(std::min<size_t>(1172, payload - offset), 24);
            boneBytes += bytes.size(); logical += control + bytes.size();
            if (key) ++keys; else ++deltas;
        }
        if (datagram > 4) after += datagram + 72;
    }
    // Initial world dictionaries/control baselines, plus ordinary player state
    // traffic. Stable descriptors need no repeated baseline offers in this scene.
    const uint64_t bootstrap = count * (37 + 615 + 611 + 2 * (2855 + 300)) + 341 + 81 * 101;
    // Two reliable contact/life/control transitions per fighter per second:
    // full essential record, baseline offer, ID and conservative ENet/relay envelope.
    const uint64_t criticalControls = seconds * 5 * 2 * (611 + 615 + 37 + 200);
    const uint64_t allowances = bootstrap + criticalControls + seconds * 60 * (402 + 8 + 4 + 72);
    after += allowances;
    std::printf("75-NPC synthetic 30 Hz ordinary, 20s, exact affine poses, 17 ms ACK delay, 5%% pose/ACK loss: "
                "baseline %.0f B/s, rate-only %.0f B/s, content/unbatched %.0f B/s, packed+bootstrap+controls+player %.0f B/s "
                "(pose application %.0f B/s, unbatched+same allowances %.0f B/s); "
                "updates/s %.1f, fast/ordinary/far/idle %.1f/%.1f/%.1f/%.1f; bone bytes/update %.1f keys=%llu deltas=%llu\n",
                double(before) / seconds, double(rateOnly) / seconds, double(unbatched) / seconds,
                double(after) / seconds, double(logical) / seconds, double(unbatched + allowances) / seconds,
                double(sends) / seconds, double(byTier[0]) / seconds, double(byTier[1]) / seconds,
                double(byTier[2]) / seconds, double(byTier[3]) / seconds, double(boneBytes) / sends,
                static_cast<unsigned long long>(keys), static_cast<unsigned long long>(deltas));
    std::fflush(stdout);
    std::printf("Advertised-tier fast/ordinary/slow updates/s %.1f/%.1f/%.1f (matches live pose log classification)\n",
                double(advertised[0]) / seconds, double(advertised[1]) / seconds, double(advertised[2]) / seconds);
    assert(after < before && after / seconds < 1000000);
}

int main()
{
    Codec();
    Recovery();
    IndependentRiceKeys();
    ReliableAnchors();
    SecondOrderSpatial();
    LongDelayedFirstAnchor();
    Simulation();
}
