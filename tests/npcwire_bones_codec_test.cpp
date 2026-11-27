// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcwire_bones.h"

#include <cassert>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace wire = g3t::npcwire::bones;
using g3t::pose::Bone;
using wire::Dictionary;
using wire::Status;
using Bytes = std::vector<uint8_t>;

std::vector<Bone> MakeBones(std::size_t count)
{
    std::vector<Bone> result(count);
    for (std::size_t i = 0; i < count; ++i) {
        result[i].name = static_cast<uint32_t>(i + 1);
        for (std::size_t j = 0; j < 12; ++j)
            result[i].values[j] = static_cast<int16_t>(static_cast<int>(i * 13 + j) - 2000);
    }
    return result;
}

std::vector<uint32_t> Hashes(std::vector<Bone> const& bones)
{
    std::vector<uint32_t> result;
    for (auto const& bone : bones) result.push_back(bone.name);
    return result;
}

void EqualBones(std::vector<Bone> const& actual, std::vector<Bone> const& expected)
{
    assert(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        assert(actual[i].name == expected[i].name);
        for (std::size_t j = 0; j < 12; ++j) assert(actual[i].values[j] == expected[i].values[j]);
    }
}

void Golden()
{
    Dictionary sender, receiver;
    assert(sender.Reset(0x04030201u, 0x1817161514131211ull) == Status::Ok);
    assert(receiver.Reset(0x04030201u, 0x1817161514131211ull) == Status::Ok);
    const uint32_t hashes[] = {0x11223344u, 0x89abcdefu};
    assert(sender.Register(0x3412u, hashes, 2) == Status::Ok);
    std::vector<Bone> input(2);
    input[0].name = hashes[0];
    input[1].name = hashes[1];
    const int16_t values[] = {-32768, -32767, -1, 0, 1, 32767, 0x1234, -0x1234, 255, -256, 256, -255};
    for (std::size_t i = 0; i < 12; ++i) {
        input[0].values[i] = values[i];
        input[1].values[i] = values[11 - i];
    }
    const Bytes header = {74, 1, 0, 0, 1, 2, 3, 4,
                          0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x12, 0x34};
    Bytes dictionary;
    assert(sender.EncodeDictionary(0x3412u, dictionary) == Status::Ok);
    Bytes expected = header;
    expected.insert(expected.end(), {2, 0, 0x44, 0x33, 0x22, 0x11, 0xef, 0xcd, 0xab, 0x89});
    assert(dictionary == expected);
    assert(receiver.DecodeDictionary(dictionary.data(), dictionary.size()) == Status::Ok);

    Bytes encoded;
    assert(sender.EncodePose(0x3412u, input.data(), input.size(), encoded) == Status::Ok);
    expected = {0x12, 0x34};
    expected.insert(expected.end(), {
        0x00, 0x80, 0x01, 0x80, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00, 0xff, 0x7f,
        0x34, 0x12, 0xcc, 0xed, 0xff, 0x00, 0x00, 0xff, 0x00, 0x01, 0x01, 0xff,
        0x01, 0xff, 0x00, 0x01, 0x00, 0xff, 0xff, 0x00, 0xcc, 0xed, 0x34, 0x12,
        0xff, 0x7f, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01, 0x80, 0x00, 0x80});
    assert(encoded == expected);
    assert(encoded.size() == sizeof(wire::PoseHeader) + 2 * 24);
    uint16_t id = 0;
    std::vector<Bone> output;
    assert(receiver.DecodePose(encoded.data(), encoded.size(), output, id) == Status::Ok);
    assert(id == 0x3412u);
    EqualBones(output, input);
    Bytes ack, request;
    assert(receiver.EncodeAck(id, ack) == Status::Ok);
    expected = header; expected[1] = 2;
    assert(ack == expected && sender.DecodeAck(ack.data(), ack.size(), id) == Status::Ok);
    assert(wire::EncodeRequest(0x04030201u, id, request) == Status::Ok);
    assert(request == Bytes({1, 2, 3, 4, 0x12, 0x34}));
    assert(sender.RespondToRequest(request.data(), request.size(), expected) == Status::Ok && expected == dictionary);
}

void Recovery()
{
    Dictionary host, late;
    const auto input = MakeBones(3);
    const auto hashes = Hashes(input);
    assert(host.Reset(8, 41) == Status::Ok);
    assert(late.Reset(8, 41) == Status::Ok);
    assert(host.Register(25, hashes.data(), hashes.size()) == Status::Ok);
    Bytes pose, dictionary, request, response;
    assert(host.EncodePose(25, input.data(), input.size(), pose) == Status::Ok);
    assert(host.EncodeDictionary(25, dictionary) == Status::Ok);
    std::vector<Bone> output = MakeBones(1);
    const auto original = output;
    uint16_t id = 777;
    // The definition was lost, or the unreliable pose arrived first.
    assert(late.DecodePose(pose.data(), pose.size(), output, id) == Status::UnknownLayout);
    assert(id == 25);
    EqualBones(output, original);
    assert(wire::EncodeRequest(8, id, request) == Status::Ok);
    assert(host.RespondToRequest(request.data(), request.size(), response) == Status::Ok);
    assert(response == dictionary);
    assert(late.DecodeDictionary(response.data(), response.size()) == Status::Ok);
    assert(late.DecodeDictionary(dictionary.data(), dictionary.size()) == Status::Ok);
    assert(late.Reset(8, 41) == Status::Ok);
    assert(late.DecodePose(pose.data(), pose.size(), output, id) == Status::Ok);
    EqualBones(output, input);

    // Independent peers and layout IDs remain separate even with out-of-order definitions.
    auto other = input;
    other[0].name = 987;
    const auto otherHashes = Hashes(other);
    assert(host.Register(26, otherHashes.data(), otherHashes.size()) == Status::Ok);
    Bytes second;
    assert(host.EncodeDictionary(26, second) == Status::Ok);
    Dictionary newcomer;
    assert(newcomer.Reset(8, 41) == Status::Ok);
    assert(newcomer.DecodeDictionary(second.data(), second.size()) == Status::Ok);
    assert(newcomer.DecodePose(pose.data(), pose.size(), output, id) == Status::UnknownLayout);
    assert(newcomer.DecodeDictionary(dictionary.data(), dictionary.size()) == Status::Ok);
    assert(newcomer.DecodePose(pose.data(), pose.size(), output, id) == Status::Ok);
    EqualBones(output, input);

    assert(wire::EncodeRequest(8, 500, request) == Status::Ok);
    response = {77};
    assert(host.RespondToRequest(request.data(), request.size(), response) == Status::UnknownLayout);
    assert(response == Bytes{77});
    assert(host.Register(25, hashes.data(), hashes.size()) == Status::Ok);
    assert(host.Register(25, otherHashes.data(), otherHashes.size()) == Status::Conflict);
    assert(host.EncodePose(25, other.data(), other.size(), response) == Status::Conflict);
    assert(response == Bytes{77});
    uint16_t found = 999;
    assert(host.FindLayout(hashes.data(), hashes.size(), found) == Status::Ok && found == 25);
    const uint32_t absent[] = {4, 3, 2, 1};
    assert(host.FindLayout(absent, 4, found) == Status::UnknownLayout && found == 25);

    // A request from a peer still on the previous generation reaches the new one.
    Bytes oldAck, oldRequest;
    assert(late.EncodeAck(25, oldAck) == Status::Ok);
    assert(wire::EncodeRequest(8, 25, oldRequest) == Status::Ok);
    assert(host.Reset(8, 42) == Status::Ok);
    assert(host.Register(27, otherHashes.data(), otherHashes.size()) == Status::Ok);
    assert(host.RespondToRequest(oldRequest.data(), oldRequest.size(), response) == Status::UnknownLayout);
    assert(host.DecodeAck(oldAck.data(), oldAck.size(), id) == Status::WrongGeneration);
    assert(host.EncodePose(27, other.data(), other.size(), second) == Status::Ok);
    output = original;
    assert(late.DecodePose(second.data(), second.size(), output, id) == Status::UnknownLayout && id == 27);
    EqualBones(output, original);
    assert(wire::EncodeRequest(8, id, request) == Status::Ok);
    assert(host.RespondToRequest(request.data(), request.size(), second) == Status::Ok);
    assert(late.DecodeDictionary(second.data(), second.size()) == Status::WrongGeneration);
    assert(late.Reset(8, 42) == Status::Ok);
    assert(!late.HasLayout(25));
    assert(late.DecodeDictionary(dictionary.data(), dictionary.size()) == Status::WrongGeneration);
    assert(late.DecodeDictionary(second.data(), second.size()) == Status::Ok);
    assert(late.Reset(9, 42) == Status::Ok);
    assert(!late.HasLayout(27));
    assert(late.DecodeDictionary(second.data(), second.size()) == Status::WrongGeneration);
    assert(late.Reset(0, 42) == Status::InvalidArgument);
    late.Clear();
    assert(late.DecodeDictionary(second.data(), second.size()) == Status::NotInitialized);
}

void Malformed()
{
    Dictionary sender, receiver;
    assert(sender.Reset(1, 2) == Status::Ok && receiver.Reset(1, 2) == Status::Ok);
    auto bones = MakeBones(2);
    auto hashes = Hashes(bones);
    assert(sender.Register(9, hashes.data(), hashes.size()) == Status::Ok);
    Bytes dict, pose, request;
    assert(sender.EncodeDictionary(9, dict) == Status::Ok);
    assert(sender.EncodePose(9, bones.data(), bones.size(), pose) == Status::Ok);
    assert(wire::EncodeRequest(1, 9, request) == Status::Ok);
    for (std::size_t n = 0; n < dict.size(); ++n) {
        assert(receiver.DecodeDictionary(dict.data(), n) != Status::Ok);
        assert(!receiver.HasLayout(9));
    }
    assert(receiver.DecodeDictionary(dict.data(), dict.size()) == Status::Ok);
    for (std::size_t n = 0; n < pose.size(); ++n) {
        uint16_t id = 123;
        auto output = MakeBones(1);
        const auto before = output;
        assert(receiver.DecodePose(pose.data(), n, output, id) != Status::Ok);
        assert(id == 123);
        EqualBones(output, before);
    }
    Bytes answer{91};
    for (std::size_t n = 0; n < request.size(); ++n) {
        assert(sender.RespondToRequest(request.data(), n, answer) != Status::Ok);
        assert(answer == Bytes{91});
    }
    assert(receiver.DecodeDictionary(nullptr, dict.size()) == Status::Malformed);
    assert(sender.RespondToRequest(nullptr, request.size(), answer) == Status::Malformed);
    uint16_t id = 123;
    std::vector<Bone> output;
    assert(receiver.DecodePose(nullptr, pose.size(), output, id) == Status::Malformed && id == 123);
    for (std::size_t index : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(3)}) {
        auto bad = dict;
        bad[index] ^= 0x80;
        assert(receiver.DecodeDictionary(bad.data(), bad.size()) == Status::Malformed);
        assert(receiver.HasLayout(9));
    }
    auto bad = dict;
    bad.push_back(0);
    assert(receiver.DecodeDictionary(bad.data(), bad.size()) == Status::Malformed);
    bad = dict; bad[18] = 0; bad[19] = 0;
    assert(receiver.DecodeDictionary(bad.data(), bad.size()) == Status::Malformed);
    bad = dict; bad[18] = 1; bad[19] = 1; // 257
    assert(receiver.DecodeDictionary(bad.data(), bad.size()) == Status::Malformed);
    bad = dict; std::memset(bad.data() + 20, 0, 4);
    assert(receiver.DecodeDictionary(bad.data(), bad.size()) == Status::Malformed);
    bad = dict; std::memcpy(bad.data() + 24, bad.data() + 20, 4);
    assert(receiver.DecodeDictionary(bad.data(), bad.size()) == Status::Malformed);
    bad = dict; std::memset(bad.data() + 16, 0, 2);
    assert(receiver.DecodeDictionary(bad.data(), bad.size()) == Status::Malformed);
    bad = dict; std::memset(bad.data() + 4, 0, 4);
    assert(receiver.DecodeDictionary(bad.data(), bad.size()) == Status::Malformed);
    bad = dict; std::memset(bad.data() + 8, 0, 8);
    assert(receiver.DecodeDictionary(bad.data(), bad.size()) == Status::Malformed);
    assert(receiver.DecodeDictionary(dict.data(), dict.size()) == Status::Ok);
    bad = dict; bad[20] = 3;
    assert(receiver.DecodeDictionary(bad.data(), bad.size()) == Status::Conflict);
    assert(receiver.DecodeDictionary(dict.data(), dict.size()) == Status::Ok);
    bad = pose; bad.push_back(0);
    assert(receiver.DecodePose(bad.data(), bad.size(), output, id) == Status::Malformed && id == 123);
    bad = pose; bad.resize(sizeof(wire::PoseHeader) + 24);
    assert(receiver.DecodePose(bad.data(), bad.size(), output, id) == Status::Malformed && id == 123);
    bad = pose; bad[0] = bad[1] = 0;
    assert(receiver.DecodePose(bad.data(), bad.size(), output, id) == Status::Malformed && id == 123);
    bad = request; bad.push_back(0);
    assert(sender.RespondToRequest(bad.data(), bad.size(), answer) == Status::Malformed);
    assert(answer == Bytes{91});
    bad = request; bad[0] = 2;
    assert(sender.RespondToRequest(bad.data(), bad.size(), answer) == Status::WrongGeneration);
    bad = request; bad[4] = bad[5] = 0;
    assert(sender.RespondToRequest(bad.data(), bad.size(), answer) == Status::Malformed);
    Bytes ack;
    assert(receiver.EncodeAck(9, ack) == Status::Ok);
    id = 123;
    bad = ack; bad[8] ^= 1;
    assert(sender.DecodeAck(bad.data(), bad.size(), id) == Status::WrongGeneration && id == 123);
    bad = ack; bad[16] = 10;
    assert(sender.DecodeAck(bad.data(), bad.size(), id) == Status::UnknownLayout && id == 123);
    bad = ack; bad.push_back(0);
    assert(sender.DecodeAck(bad.data(), bad.size(), id) == Status::Malformed && id == 123);
    assert(sender.DecodeAck(ack.data(), ack.size(), id) == Status::Ok && id == 9);
    assert(wire::EncodeRequest(0, 9, answer) == Status::InvalidArgument && answer == Bytes{91});
    assert(wire::EncodeRequest(1, 0, answer) == Status::InvalidArgument && answer == Bytes{91});
    assert(sender.Register(0, hashes.data(), hashes.size()) == Status::InvalidArgument);
    assert(sender.Register(1, nullptr, 2) == Status::InvalidArgument);
    const uint32_t zero[] = {1, 0}, duplicate[] = {1, 1};
    assert(sender.Register(1, zero, 2) == Status::InvalidArgument);
    assert(sender.Register(1, duplicate, 2) == Status::InvalidArgument);
    assert(sender.Register(1, hashes.data(), 0) == Status::InvalidArgument);
    assert(sender.Register(1, hashes.data(), 257) == Status::InvalidArgument);
    assert(sender.EncodePose(9, nullptr, 2, answer) == Status::InvalidArgument && answer == Bytes{91});
}

void Bounds()
{
    Dictionary sender, receiver;
    auto bones = MakeBones(g3t::pose::MaxBones);
    auto hashes = Hashes(bones);
    assert(sender.Reset(1, 1) == Status::Ok && receiver.Reset(1, 1) == Status::Ok);
    assert(sender.Register(0xffffu, hashes.data(), hashes.size()) == Status::Ok);
    Bytes definition, pose;
    assert(sender.EncodeDictionary(0xffffu, definition) == Status::Ok);
    assert(definition.size() == wire::HeaderBytes + wire::CountBytes + 256 * 4);
    assert(definition[18] == 0 && definition[19] == 1);
    assert(receiver.DecodeDictionary(definition.data(), definition.size()) == Status::Ok);
    assert(sender.EncodePose(0xffffu, bones.data(), bones.size(), pose) == Status::Ok);
    assert(pose.size() == sizeof(wire::PoseHeader) + 256 * 24);
    uint16_t id = 0;
    std::vector<Bone> output;
    assert(receiver.DecodePose(pose.data(), pose.size(), output, id) == Status::Ok);
    assert(id == 0xffffu);
    EqualBones(output, bones);

    // Exercise every possible signed 16-bit value through the exact wire representation.
    bones.resize(1);
    hashes.resize(1);
    assert(sender.Register(2, hashes.data(), 1) == Status::Ok);
    assert(sender.EncodeDictionary(2, definition) == Status::Ok);
    assert(receiver.DecodeDictionary(definition.data(), definition.size()) == Status::Ok);
    for (int value = -32768; value <= 32767; ++value) {
        for (auto& component : bones[0].values) component = static_cast<int16_t>(value);
        assert(sender.EncodePose(2, bones.data(), 1, pose) == Status::Ok);
        const uint16_t bits = static_cast<uint16_t>(value);
        assert(pose[2] == static_cast<uint8_t>(bits));
        assert(pose[3] == static_cast<uint8_t>(bits >> 8));
        assert(receiver.DecodePose(pose.data(), pose.size(), output, id) == Status::Ok);
        EqualBones(output, bones);
    }

    // Fast layout matching, Bone-based FindLayout, IdentityPosePtr, and copy/move safety.
    auto full = MakeBones(12);
    auto fullHashes = Hashes(full);
    assert(sender.Register(7, fullHashes.data(), fullHashes.size()) == Status::Ok);
    assert(sender.MatchesLayout(7, full.data(), full.size()));
    assert(!sender.MatchesLayout(7, full.data(), full.size() - 1));
    assert(!sender.MatchesLayout(8, full.data(), full.size()));
    uint16_t foundId = 0;
    assert(sender.FindLayout(full.data(), full.size(), foundId) == Status::Ok && foundId == 7);
    auto* idPtr = sender.IdentityPosePtr(7);
    assert(idPtr && idPtr->size() == 12 && (*idPtr)[0].values[0] == 4096 && (*idPtr)[0].name == full[0].name);
    Dictionary copied = sender;
    assert(copied.MatchesLayout(7, full.data(), full.size()));
    Dictionary moved = std::move(copied);
    assert(moved.MatchesLayout(7, full.data(), full.size()));
    full[3].name ^= 0x100u;
    assert(!moved.MatchesLayout(7, full.data(), full.size()));
    assert(moved.FindLayout(full.data(), full.size(), foundId) == Status::UnknownLayout);
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string test = argv[1];
    if (test == "golden") Golden();
    else if (test == "recovery") Recovery();
    else if (test == "malformed") Malformed();
    else if (test == "bounds") Bounds();
    else return 2;
    return 0;
}
