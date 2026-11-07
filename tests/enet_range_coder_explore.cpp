// SPDX-License-Identifier: GPL-3.0-or-later
#include "../thirdparty/enet/include/enet/enet.h"
#include "../src/playerwire.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void* enet_malloc(size_t size) { return std::malloc(size); }
extern "C" void enet_free(void* memory) { std::free(memory); }
extern "C" void enet_host_compress(struct _ENetHost*, ENetCompressor const*) {}

namespace {
std::vector<unsigned char> MakeCompactPlayer()
{
    g3t::net::PlayerState state{};
    state.id = 1; state.sequence = 123; state.sent = 4000; state.world = 0x12345678;
    state.flags = 1; state.position[0] = 80234.125f; state.position[1] = 4786.25f;
    state.position[2] = -12836.5f; state.rotation[1] = 0.342f; state.rotation[3] = 0.94f;
    state.movementMode = 3; state.aniState = 2; state.motionFrac = 12345; state.actionSeq = 17;
    std::strcpy(state.animation, "Walk");
    g3t::playerwire::Encoder encoder;
    g3t::playerwire::Packet packet;
    assert(encoder.Plan(state, 4000, false, packet) == g3t::playerwire::Status::Ok);
    encoder.Commit(state, 4000, packet);
    ++state.sequence; state.sent += 33; state.position[0] += 3.25f; state.motionFrac += 271;
    assert(encoder.Plan(state, 4033, false, packet) == g3t::playerwire::Status::Ok);
    std::vector<unsigned char> out{7, 0, 0, 1, 0, 1, 0, 66, 3};
    out.insert(out.end(), packet.bytes.begin(), packet.bytes.end());
    return out;
}

std::vector<unsigned char> MakeReliableEvents()
{
    g3t::net::WorldState event{};
    event.kind = 19; event.npcClass = 1; event.guid[0] = 17; event.dialog = 1;
    std::vector<unsigned char> out{0x86, 1, 0, 1, 0, 103, 23};
    auto const* data = reinterpret_cast<unsigned char const*>(&event);
    out.insert(out.end(), data, data + sizeof(event));
    return out;
}

std::vector<unsigned char> MakeSaveText()
{
    std::vector<unsigned char> out;
    static char const line[] = "world aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\nsave bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\nplayer 123\n";
    while (out.size() + sizeof(line) - 1 <= 1168)
        out.insert(out.end(), line, line + sizeof(line) - 1);
    out.resize(1168);
    return out;
}

std::vector<unsigned char> MakeLegacyState()
{
    std::vector<unsigned char> out(512);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = i % 16 == 12 ? static_cast<unsigned char>((i / 16) & 3u)
                             : static_cast<unsigned char>((i * 5u + i / 16u) & 0x3fu);
    return out;
}

std::vector<unsigned char> MakeRandom()
{
    std::vector<unsigned char> out(1168);
    std::uint32_t state = 0x12345678u;
    for (std::size_t i = 0; i < out.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        out[i] = static_cast<unsigned char>(state >> 24);
    }
    return out;
}

std::vector<unsigned char> MakeQuantized()
{
    std::vector<unsigned char> out(1168);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<unsigned char>((i / 19u) & 31u);
    return out;
}

void Run(char const* name, std::vector<unsigned char> const& input)
{
    void* context = enet_range_coder_create();
    assert(context);
    std::vector<unsigned char> compressed(input.size() + 32);
    ENetBuffer buffer;
    buffer.data = const_cast<unsigned char*>(input.data());
    buffer.dataLength = input.size();
    constexpr unsigned Iterations = 1000;
    auto started = std::chrono::steady_clock::now();
    size_t compressedSize = 0;
    for (unsigned i = 0; i < Iterations; ++i)
        compressedSize = enet_range_coder_compress(context, &buffer, 1, input.size(),
                                                   compressed.data(), input.size());
    auto compressedAt = std::chrono::steady_clock::now();
    std::vector<unsigned char> restored(input.size());
    size_t restoredSize = 0;
    if (compressedSize) {
        for (unsigned i = 0; i < Iterations; ++i)
            restoredSize = enet_range_coder_decompress(context, compressed.data(), compressedSize,
                                                       restored.data(), restored.size());
    }
    auto finished = std::chrono::steady_clock::now();
    bool exact = !compressedSize ||
                 (restoredSize == input.size() && std::memcmp(restored.data(), input.data(), input.size()) == 0);
    assert(exact);
    double encodeUs = std::chrono::duration<double, std::micro>(compressedAt - started).count() / Iterations;
    double decodeUs = std::chrono::duration<double, std::micro>(finished - compressedAt).count() / Iterations;
    double mb = static_cast<double>(input.size()) / (1024.0 * 1024.0);
    double encodeMBs = mb / (encodeUs / 1000000.0);
    double decodeMBs = compressedSize && decodeUs > 0 ? mb / (decodeUs / 1000000.0) : 0.0;
    size_t afterCompaction = compressedSize && compressedSize < input.size() ? compressedSize : input.size();
    double saving = (100.0 * (static_cast<double>(input.size()) - afterCompaction) / input.size());
    std::printf("%s input=%zu output=%zu%s saving=%.2f%% encode_us=%.1f decode_us=%.1f "
                "encode_MBps=%.1f decode_MBps=%.1f roundtrip=%s\n",
                name, input.size(), afterCompaction, compressedSize ? "" : "(range-coder-no-fit)",
                saving, encodeUs, decodeUs,
                encodeMBs, decodeMBs, exact ? "exact" : "FAIL");
    enet_range_coder_destroy(context);
}
}

int main()
{
    Run("compact-player-command", MakeCompactPlayer());
    Run("world-xp-command", MakeReliableEvents());
    Run("synthetic-manifest", MakeSaveText());
    Run("legacy-comparison-only", MakeLegacyState());
    Run("incompressible-datagram", MakeRandom());
    Run("synthetic-low-entropy", MakeQuantized());
    std::puts("recommendation=disabled-representative-pose-evidence-required; "
              "fragmentation-budget=1172/1168-with-checksum; timings=1000-call-means");
    return 0;
}
