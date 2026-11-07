// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/client_rejoin.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace g3t::rejoin;

namespace {
void Policy()
{
    State state;
    Boundary idle;
    assert(!state.pending && !state.fresh && !state.failed);
    assert(!state.CanRetire(idle));
    assert(!state.CanUseFresh(true, true));
    for (bool client : {false, true})
        for (bool hooks : {false, true})
            for (bool busy : {false, true})
                assert(MenuEnabled(client, hooks, busy) == (client && hooks && !busy));

    // Joining the first host world and ordinary same-epoch reconnect never retire a hero.
    assert(!ReplacesWorld(0, 1, true, true, false));
    assert(!ReplacesWorld(1, 1, true, true, false));
    assert(!ReplacesWorld(1, 2, false, true, false));
    assert(!ReplacesWorld(1, 2, true, false, false));
    assert(ReplacesWorld(1, 2, true, true, false));
    // A loading/menu epoch must not erase a retirement queued by the preceding active epoch.
    assert(ReplacesWorld(2, 3, false, true, true));

    state.Started(true);
    assert(state.CanUseFresh(true, true));
    assert(!state.CanUseFresh(false, true));
    assert(!state.CanUseFresh(true, false)); // host loading: retain the explicit New Game, no admission
    state.Left();                         // admitted hero or user left the world
    assert(!state.CanUseFresh(true, true)); // old live/default hero cannot become a new account
    state.Queue();
    assert(state.pending && !state.fresh);
    assert(state.CanRetire(idle));
    for (unsigned mask = 1; mask < 16; ++mask) {
        Boundary blocked{(mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0};
        assert(!state.CanRetire(blocked));
        assert(state.Waiting(blocked) != Wait::None);
    }
    assert(state.Waiting({true, false, true, true}) == Wait::Native);
    assert(state.Waiting({false, true, true, true}) == Wait::Native);
    assert(state.Waiting({false, false, true, true}) == Wait::Save);
    assert(state.Waiting({false, false, false, true}) == Wait::Interaction);
    state.Queue(); // repeated host epochs are idempotent
    state.Retired();
    assert(!state.pending && !state.CanUseFresh(true, true));
    // Canceling the native confirmation never calls Started(true).
    assert(!state.fresh && !state.failed);
    state.Started(false);
    assert(state.failed && !state.CanUseFresh(true, true));
    state.Started(true); // retry after a failed New Game
    assert(!state.failed && state.CanUseFresh(true, true));
    state.Queue();      // host replacement before restoration/admission
    assert(!state.CanUseFresh(true, true));
}

void Put(std::vector<uint8_t>& image, size_t at, uint32_t word)
{
    assert(g3t::saving::native::Range(image.size(), at, 4));
    std::memcpy(image.data() + at, &word, 4);
}
void NativeFixtures()
{
    using namespace g3t::saving::native;
    std::vector<uint8_t> image(0xb0000, 0xcc);
    for (auto const& site : {native::Command, native::Loader, native::MenuGate, native::Button}) {
        const auto bytes = Decode(site.bytes);
        for (size_t i = 0; i < bytes.size(); ++i) image[site.hint + i] = bytes[i] < 0 ? 0 : static_cast<uint8_t>(bytes[i]);
    }
    image[0x90a1b] = 0xe8;
    Put(image, 0x90a1c, native::Loader.hint - 0x90a20);
    assert(native::Matches(image.data(), image.size()));
    for (auto const& site : {native::Command, native::Loader, native::MenuGate, native::Button}) {
        const auto bytes = Decode(site.bytes);
        for (size_t i = 0; i < bytes.size(); ++i) {
            image[site.hint + i] ^= 1;
            assert(native::Matches(image.data(), image.size()) == (bytes[i] < 0));
            image[site.hint + i] ^= 1;
        }
    }
    image[0x90a1c] ^= 1;
    assert(!native::Matches(image.data(), image.size()));
    assert(!native::Matches(image.data(), 10));
    assert(!native::Matches(nullptr, 0));
    static_assert(native::CommandStolen == 5 && native::LoaderStolen == 7);
}
uint32_t Word(std::vector<uint8_t> const& bytes, size_t at)
{
    if (!g3t::saving::native::Range(bytes.size(), at, 4)) throw std::runtime_error("truncated PE");
    return g3t::saving::native::Word(bytes.data(), at);
}
uint16_t Short(std::vector<uint8_t> const& bytes, size_t at)
{
    if (!g3t::saving::native::Range(bytes.size(), at, 2)) throw std::runtime_error("truncated PE");
    uint16_t value;
    std::memcpy(&value, bytes.data() + at, 2);
    return value;
}
void RealGame(char const* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read configured Game.dll");
    const std::vector<uint8_t> disk{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const size_t pe = Word(disk, 0x3c), optional = pe + 24;
    assert(Short(disk, 0) == 0x5a4d && Word(disk, pe) == 0x4550);
    assert(Short(disk, pe + 4) == 0x14c && Short(disk, optional) == 0x10b);
    const uint32_t size = Word(disk, optional + 56), headers = Word(disk, optional + 60);
    assert(size <= 128u * 1024 * 1024 && headers <= size && headers <= disk.size());
    std::vector<uint8_t> image(size, 0);
    std::copy_n(disk.begin(), headers, image.begin());
    const size_t sections = optional + Short(disk, pe + 20);
    for (unsigned i = 0; i < Short(disk, pe + 6); ++i) {
        const size_t section = sections + i * 40;
        const uint32_t rva = Word(disk, section + 12), length = Word(disk, section + 16), raw = Word(disk, section + 20);
        assert(g3t::saving::native::Range(disk.size(), raw, length));
        assert(g3t::saving::native::Range(image.size(), rva, length));
        std::copy_n(disk.begin() + raw, length, image.begin() + rva);
    }
    assert(native::Matches(image.data(), image.size()));
    // Apply real PE HIGHLOW relocations: masked operands tolerate ASLR; the rel32 loader edge stays exact.
    const size_t reloc = Word(disk, optional + 136), end = reloc + Word(disk, optional + 140);
    for (size_t block = reloc; block < end;) {
        const uint32_t page = Word(image, block), length = Word(image, block + 4);
        assert(length >= 8 && g3t::saving::native::Range(end, block, length));
        for (size_t at = block + 8; at < block + length; at += 2) {
            const uint16_t entry = Short(image, at);
            if (entry >> 12 == 3) Put(image, page + (entry & 0xfff), Word(image, page + (entry & 0xfff)) + 0x25000000);
        }
        block += length;
    }
    assert(native::Matches(image.data(), image.size()));
}
}

int main(int argc, char** argv)
{
    Policy();
    NativeFixtures();
    if (argc == 2) RealGame(argv[1]);
    return 0;
}
