// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#define G3T_PLAYERWIRE_HAS_PLAYERSTATE 1
namespace g3t::net {
struct PlayerState {
    std::uint8_t id = 0;
    std::uint32_t sequence = 0;
    std::uint32_t sent = 0;
    std::uint32_t world = 0;
    float position[3] = {};
    float rotation[4] = {};
    std::uint8_t flags = 0;
    std::uint8_t movementMode = 0;
    std::uint8_t aniState = 0;
    char name[24] = {};
    char animation[96] = {};
    char heldRight[40] = {};
    char heldLeft[40] = {};
    char shape[32] = {};
    char armor[40] = {};
    char helmet[40] = {};
    char look[32] = {};
    std::int32_t gold = 0;
    std::uint8_t actionSeq = 0;
    std::uint16_t actionMs = 0;
    std::uint16_t motionFrac = 65535;
    std::uint32_t downedSequence = 0;
    std::uint32_t focusNpc = 0;
    std::uint32_t targetNpc = 0;
};
}
#include "../src/playerwire.h"

namespace {

void setName(char* output, std::size_t size, const char* text) {
    std::memset(output, 0, size);
    std::strncpy(output, text, size - 1);
}

g3t::net::PlayerState sample(std::uint32_t sequence = 100) {
    g3t::net::PlayerState state;
    state.id = 2;
    state.sequence = sequence;
    state.sent = 0xfedc1234u;
    state.world = 0x89abcdefu;
    state.position[0] = -9999999.0f;
    state.position[1] = 2345.5f;
    state.position[2] = 9999999.0f;
    state.rotation[0] = 0.1f;
    state.rotation[1] = -0.2f;
    state.rotation[2] = 0.3f;
    state.rotation[3] = 0.9f;
    state.flags = 0x1f;
    state.movementMode = 7;
    state.aniState = 18;
    setName(state.name, sizeof(state.name), "Alice");
    setName(state.animation, sizeof(state.animation), "s_run");
    setName(state.heldRight, sizeof(state.heldRight), "1h_sword");
    setName(state.heldLeft, sizeof(state.heldLeft), "iron_shield");
    setName(state.shape, sizeof(state.shape), "Wolf");
    setName(state.armor, sizeof(state.armor), "plate_chest");
    setName(state.helmet, sizeof(state.helmet), "plate_helmet");
    setName(state.look, sizeof(state.look), "hero_look");
    state.gold = 123456;
    state.actionSeq = 221;
    state.actionMs = 1234;
    state.motionFrac = 54321;
    state.downedSequence = 0xabcdef01u;
    state.focusNpc = 0xdeadbeefu;
    state.targetNpc = 0x01234567u;
    return state;
}

void assertState(const g3t::net::PlayerState& a, const g3t::net::PlayerState& b) {
    assert(a.id == b.id);
    assert(a.sequence == b.sequence);
    assert(a.sent == b.sent);
    assert(a.world == b.world);
    for (unsigned i = 0; i < 3; ++i) assert(a.position[i] == b.position[i]);
    for (unsigned i = 0; i < 4; ++i) assert(a.rotation[i] == b.rotation[i]);
    assert(a.flags == b.flags);
    assert(a.movementMode == b.movementMode);
    assert(a.aniState == b.aniState);
    assert(std::strcmp(a.name, b.name) == 0);
    assert(std::strcmp(a.animation, b.animation) == 0);
    assert(std::strcmp(a.heldRight, b.heldRight) == 0);
    assert(std::strcmp(a.heldLeft, b.heldLeft) == 0);
    assert(std::strcmp(a.shape, b.shape) == 0);
    assert(std::strcmp(a.armor, b.armor) == 0);
    assert(std::strcmp(a.helmet, b.helmet) == 0);
    assert(std::strcmp(a.look, b.look) == 0);
    assert(a.gold == b.gold);
    assert(a.actionSeq == b.actionSeq);
    assert(a.actionMs == b.actionMs);
    assert(a.motionFrac == b.motionFrac);
    assert(a.downedSequence == b.downedSequence);
    assert(a.focusNpc == b.focusNpc);
    assert(a.targetNpc == b.targetNpc);
}

void testRoundtripMasksAndCommitPreview() {
    g3t::playerwire::Encoder encoder;
    g3t::net::PlayerState state = sample();

    g3t::playerwire::Packet preview;
    assert(encoder.Plan(state, 1000, false, preview) == g3t::playerwire::Status::Ok);
    assert(preview.full);
    assert(preview.reliable);
    assert(preview.revision == 1);
    assert(preview.bytes.size() > g3t::playerwire::FixedBytes);
    g3t::playerwire::Packet previewAgain;
    assert(encoder.Plan(state, 1000, false, previewAgain) == g3t::playerwire::Status::Ok);
    assert(previewAgain.bytes == preview.bytes);
    assert(!encoder.HaveBaseline());
    encoder.Commit(state, 1000, preview);

    g3t::playerwire::Decoder decoder;
    g3t::net::PlayerState restored;
    assert(decoder.Decode(preview.bytes.data(), preview.bytes.size(), restored) == g3t::playerwire::Status::Ok);
    assertState(state, restored);
    assert(decoder.HasKeyframe());
    assert(decoder.Revision() == 1);
    assert(decoder.Sequence() == 100);

    g3t::net::PlayerState hot = state;
    hot.sequence = 101;
    hot.position[1] += 3.25f;
    hot.sent++;
    g3t::playerwire::Packet hotPacket;
    assert(encoder.Plan(hot, 1033, false, hotPacket) == g3t::playerwire::Status::Ok);
    assert(!hotPacket.full);
    assert(!hotPacket.reliable);
    assert(hotPacket.bytes.size() == g3t::playerwire::FixedBytes);
    encoder.Commit(hot, 1033, hotPacket);
    assert(decoder.Decode(hotPacket.bytes.data(), hotPacket.bytes.size(), restored) == g3t::playerwire::Status::Ok);
    assert(restored.sequence == 101);
    assert(restored.position[1] == hot.position[1]);

    g3t::net::PlayerState metadata = hot;
    metadata.sequence = 102;
    setName(metadata.name, sizeof(metadata.name), "Bob");
    metadata.focusNpc = 0x12345678u;
    g3t::playerwire::Packet metadataPacket;
    assert(encoder.Plan(metadata, 1066, false, metadataPacket) == g3t::playerwire::Status::Ok);
    assert(metadataPacket.reliable);
    assert(!metadataPacket.full);
    assert(metadataPacket.revision == 2);
    assert(metadataPacket.bytes.size() > g3t::playerwire::FixedBytes);
    encoder.Commit(metadata, 1066, metadataPacket);
    assert(decoder.Decode(metadataPacket.bytes.data(), metadataPacket.bytes.size(), restored) == g3t::playerwire::Status::Ok);
    assert(std::strcmp(restored.name, "Bob") == 0);
    assert(restored.focusNpc == metadata.focusNpc);
    assert(decoder.Revision() == 2);

    g3t::net::PlayerState reliable = metadata;
    reliable.sequence = 103;
    reliable.flags ^= 1;
    reliable.downedSequence++;
    reliable.world++;
    reliable.actionSeq++;
    g3t::playerwire::Packet reliablePacket;
    assert(encoder.Plan(reliable, 1100, false, reliablePacket) == g3t::playerwire::Status::Ok);
    assert(reliablePacket.reliable);
    encoder.Commit(reliable, 1100, reliablePacket);

    g3t::net::PlayerState cadenceState = reliable;
    cadenceState.sequence = 104;
    g3t::playerwire::Packet beforeKeyframe;
    assert(encoder.Plan(cadenceState, 5999, false, beforeKeyframe) == g3t::playerwire::Status::Ok);
    assert(!beforeKeyframe.full);
    g3t::playerwire::Packet keyframe;
    assert(encoder.Plan(cadenceState, 6000, false, keyframe) == g3t::playerwire::Status::Ok);
    assert(keyframe.full);
    assert(keyframe.reliable);
    assert(keyframe.bytes.size() > g3t::playerwire::FixedBytes);

    g3t::playerwire::Packet stale = hotPacket;
    g3t::net::PlayerState staleOut;
    assert(decoder.Decode(stale.bytes.data(), stale.bytes.size(), staleOut) == g3t::playerwire::Status::Stale);
}

void testLossRecoveryAndStale() {
    g3t::playerwire::Encoder encoder;
    g3t::net::PlayerState state = sample(100);
    g3t::playerwire::Packet fullPacket;
    assert(encoder.Plan(state, 0, false, fullPacket) == g3t::playerwire::Status::Ok);
    encoder.Commit(state, 0, fullPacket);

    g3t::playerwire::Decoder recovery;
    g3t::net::PlayerState out;
    assert(recovery.Decode(fullPacket.bytes.data(), fullPacket.bytes.size(), out) == g3t::playerwire::Status::Ok);

    g3t::net::PlayerState changed = state;
    changed.sequence = 102;
    setName(changed.name, sizeof(changed.name), "DroppedName");
    g3t::playerwire::Packet metadataPacket;
    assert(encoder.Plan(changed, 33, false, metadataPacket) == g3t::playerwire::Status::Ok);
    assert(metadataPacket.revision == 2);
    encoder.Commit(changed, 33, metadataPacket);

    g3t::net::PlayerState later = changed;
    later.sequence = 103;
    later.position[0] += 1.0f;
    g3t::playerwire::Packet laterPacket;
    assert(encoder.Plan(later, 66, false, laterPacket) == g3t::playerwire::Status::Ok);
    encoder.Commit(later, 66, laterPacket);
    assert(recovery.Decode(laterPacket.bytes.data(), laterPacket.bytes.size(), out) == g3t::playerwire::Status::NeedKeyframe);

    later.sequence = 104;
    g3t::playerwire::Packet recoveryPacket;
    assert(encoder.Plan(later, 5001, true, recoveryPacket) == g3t::playerwire::Status::Ok);
    assert(recoveryPacket.full);
    encoder.Commit(later, 5001, recoveryPacket);
    assert(recovery.Decode(recoveryPacket.bytes.data(), recoveryPacket.bytes.size(), out) == g3t::playerwire::Status::Ok);
    assert(out.sequence == 104);
    assert(recovery.Revision() == recoveryPacket.revision);

    g3t::net::PlayerState staleOut = out;
    assert(recovery.Decode(fullPacket.bytes.data(), fullPacket.bytes.size(), staleOut) == g3t::playerwire::Status::Stale);
    assert(staleOut.sequence == out.sequence);

    g3t::playerwire::Decoder untouched;
    g3t::net::PlayerState need;
    assert(untouched.Decode(laterPacket.bytes.data(), laterPacket.bytes.size(), need) == g3t::playerwire::Status::NeedKeyframe);
    assert(untouched.Decode(fullPacket.bytes.data(), fullPacket.bytes.size(), need) == g3t::playerwire::Status::Ok);
}

void testFullKeyframeCadenceAndSequenceWrap() {
    g3t::playerwire::Encoder encoder;
    g3t::net::PlayerState state = sample(0xfffffffeu);
    g3t::playerwire::Packet packet;
    assert(encoder.Plan(state, 0, false, packet) == g3t::playerwire::Status::Ok);
    encoder.Commit(state, 0, packet);

    g3t::playerwire::Decoder decoder;
    g3t::net::PlayerState out;
    assert(decoder.Decode(packet.bytes.data(), packet.bytes.size(), out) == g3t::playerwire::Status::Ok);

    state.sequence = 0xffffffffu;
    state.position[0] = 1.0f;
    assert(encoder.Plan(state, 1, false, packet) == g3t::playerwire::Status::Ok);
    encoder.Commit(state, 1, packet);
    assert(decoder.Decode(packet.bytes.data(), packet.bytes.size(), out) == g3t::playerwire::Status::Ok);

    state.sequence = 0;
    state.position[0] = 2.0f;
    assert(encoder.Plan(state, 34, false, packet) == g3t::playerwire::Status::Ok);
    encoder.Commit(state, 34, packet);
    assert(decoder.Decode(packet.bytes.data(), packet.bytes.size(), out) == g3t::playerwire::Status::Ok);
    assert(out.sequence == 0);

    g3t::net::PlayerState stale = state;
    stale.sequence = 0xffffffffu;
    stale.position[0] = 3.0f;
    g3t::playerwire::Packet stalePacket;
    g3t::playerwire::Encoder staleEncoder;
    staleEncoder.Commit(sample(0xfffffffeu), 0, packet);
    assert(staleEncoder.Plan(stale, 35, false, stalePacket) == g3t::playerwire::Status::Ok);
    assert(decoder.Decode(stalePacket.bytes.data(), stalePacket.bytes.size(), out) == g3t::playerwire::Status::Stale);

    g3t::playerwire::Cadence cadence;
    unsigned sends = 0;
    for (std::uint32_t now = 0; now <= 10000; ++now)
        if (cadence.Due(now, 30)) ++sends;
    assert(sends == 300);

    g3t::playerwire::Cadence sixty;
    unsigned sixtySends = 0;
    for (std::uint32_t now = 0; now <= 10000; ++now)
        if (sixty.Due(now, 60)) ++sixtySends;
    assert(sixtySends == 600);

    g3t::playerwire::Cadence backward;
    assert(!backward.Due(1000, 30));
    assert(!backward.Due(999, 30));
    unsigned backwardSends = 0;
    for (std::uint32_t now = 1001; now <= 4001; ++now)
        if (backward.Due(now, 30)) ++backwardSends;
    assert(backwardSends >= 90);

    g3t::playerwire::Cadence changed;
    assert(!changed.Due(100, 30));
    assert(changed.Due(133, 30));
    assert(!changed.Due(134, 60));
    assert(changed.Due(150, 60));

    g3t::playerwire::Encoder cadenceEncoder;
    g3t::net::PlayerState unchanged = sample();
    g3t::playerwire::Packet hot;
    assert(cadenceEncoder.Plan(unchanged, 0, false, hot) == g3t::playerwire::Status::Ok);
    cadenceEncoder.Commit(unchanged, 0, hot);
    std::size_t bytesPerTenSeconds = hot.bytes.size();
    (void)bytesPerTenSeconds;
}

void testMalformedBoundsAndAuthoritativeId() {
    g3t::playerwire::Encoder encoder;
    g3t::net::PlayerState state = sample();
    g3t::playerwire::Packet packet;
    assert(encoder.Plan(state, 0, false, packet) == g3t::playerwire::Status::Ok);
    encoder.Commit(state, 0, packet);

    g3t::playerwire::Decoder decoder;
    g3t::net::PlayerState out;
    assert(decoder.Decode(packet.bytes.data(), packet.bytes.size(), out) == g3t::playerwire::Status::Ok);

    std::vector<std::uint8_t> bad = packet.bytes;
    bad[0] = 2;
    assert(decoder.Decode(bad.data(), bad.size(), out) == g3t::playerwire::Status::Invalid);
    bad = packet.bytes;
    bad[63] = 0xff;
    bad[64] = 0x04;
    assert(decoder.Decode(bad.data(), bad.size(), out) == g3t::playerwire::Status::Invalid);
    bad = packet.bytes;
    bad.push_back(0);
    assert(decoder.Decode(bad.data(), bad.size(), out) == g3t::playerwire::Status::Invalid);

    bad = packet.bytes;
    bad[50] = 0;
    bad[51] = 0;
    bad[52] = 0;
    bad[53] = 0x80;
    assert(decoder.Decode(bad.data(), bad.size(), out) == g3t::playerwire::Status::Invalid);
    bad = packet.bytes;
    bad[19] = 0;
    bad[20] = 0;
    bad[21] = 0x80;
    bad[22] = 0x7f;
    assert(decoder.Decode(bad.data(), bad.size(), out) == g3t::playerwire::Status::Invalid);
    assert(decoder.Decode(packet.bytes.data(), packet.bytes.size(), out, 3) == g3t::playerwire::Status::Invalid);
    g3t::playerwire::Decoder authoritative;
    assert(authoritative.Decode(packet.bytes.data(), packet.bytes.size(), out, 2) == g3t::playerwire::Status::Ok);
    std::uint8_t missing = 0;
    assert(!g3t::playerwire::PeekPlayerId(nullptr, 0, missing));

    g3t::net::PlayerState invalid = state;
    invalid.gold = -1;
    g3t::playerwire::Packet invalidPacket;
    assert(encoder.Plan(invalid, 1, false, invalidPacket) == g3t::playerwire::Status::Invalid);
    invalid = state;
    invalid.position[0] = std::numeric_limits<float>::infinity();
    assert(encoder.Plan(invalid, 1, false, invalidPacket) == g3t::playerwire::Status::Invalid);
    invalid = state;
    std::memset(invalid.name, 'x', sizeof(invalid.name));
    assert(encoder.Plan(invalid, 1, false, invalidPacket) == g3t::playerwire::Status::Invalid);

    std::uint8_t player = 0;
    assert(g3t::playerwire::PeekPlayerId(packet.bytes.data(), packet.bytes.size(), player));
    assert(player == 2);
}

} // namespace

int main(int argc, char** argv) {
    const char* caseName = argc > 1 ? argv[1] : "all";
    if (std::strcmp(caseName, "roundtrip") == 0) testRoundtripMasksAndCommitPreview();
    else if (std::strcmp(caseName, "recovery") == 0) testLossRecoveryAndStale();
    else if (std::strcmp(caseName, "wrap") == 0) testFullKeyframeCadenceAndSequenceWrap();
    else if (std::strcmp(caseName, "malformed") == 0) testMalformedBoundsAndAuthoritativeId();
    else {
        testRoundtripMasksAndCommitPreview();
        testLossRecoveryAndStale();
        testFullKeyframeCadenceAndSequenceWrap();
        testMalformedBoundsAndAuthoritativeId();
    }
    return 0;
}
