// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/saving_native.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
using namespace g3t::saving::native;
void Put(std::vector<uint8_t>& image, size_t at, uint32_t value)
{
    assert(Range(image.size(), at, 4));
    std::memcpy(image.data() + at, &value, 4);
}
uint16_t Short(std::vector<uint8_t> const& image, size_t at)
{
    if (!Range(image.size(), at, 2)) throw std::runtime_error("truncated PE word");
    uint16_t value;
    std::memcpy(&value, image.data() + at, 2);
    return value;
}
uint32_t Get(std::vector<uint8_t> const& image, size_t at)
{
    if (!Range(image.size(), at, 4)) throw std::runtime_error("truncated PE dword");
    return Word(image.data(), at);
}
void Fixture()
{
    static_assert(Stolen == 6);
    const std::vector<int> prologue{0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    for (size_t i = 0; i < 4; ++i) {
        const auto bytes = Decode(Sites[i].bytes);
        assert(bytes.size() >= Stolen && std::equal(prologue.begin(), prologue.end(), bytes.begin()));
    }
    const uint32_t base = 0x31000000;
    std::vector<uint8_t> image(4096, 0xcc);
    size_t sites[5]{0x100, 0x300, 0x700, 0x740, 0x800};
    for (size_t i = 0; i < 5; ++i) {
        const auto pattern = Decode(Sites[i].bytes);
        for (size_t b = 0; b < pattern.size(); ++b)
            image[sites[i] + b] = pattern[b] < 0 ? 0 : static_cast<uint8_t>(pattern[b]);
        unsigned count = 0;
        assert(FindUnique(image.data(), image.size(), Sites[i].bytes, count) == sites[i] && count == 1);
    }
    for (auto pattern : {"", "5", "zz", "?5", "55?"}) {
        unsigned count = 1;
        assert(FindUnique(image.data(), image.size(), pattern, count) == Missing && count == 0);
    }
    assert(!Range(4, 5, 0) && !Range(4, 3, 2) && Range(4, 4, 0));
    for (size_t i = 0; i < 2; ++i) {
        const size_t call = sites[i] + (i ? 0x2b2 : 0xb9);
        image[call] = 0xe8;
        Put(image, call + 1, static_cast<uint32_t>(sites[i + 2] - call - 5));
    }
    const size_t getter = sites[4];
    Put(image, getter + 7, base + 0x980);
    Put(image, getter + 15, base + 0x980);
    Put(image, getter + 28, base + 0x900);
    Put(image, getter + 32, base + 0xa00);
    Put(image, getter + 37, base + 0x904);
    Put(image, getter + 42, base + 0x908);
    Put(image, getter + 55, base + 0x900);
    Put(image, 0xa00, base + 0x100);
    size_t owner = 0, table = 0;
    assert(Links(image.data(), image.size(), base, sites, owner, table) && owner == 0x900 && table == 0xa00);
    assert(!Links(image.data(), image.size(), 0x10000000, sites, owner, table));
    auto corrupt = image;
    corrupt[sites[0] + 0xb9] = 0x90;
    assert(!Links(corrupt.data(), corrupt.size(), base, sites, owner, table));
    corrupt = image;
    Put(corrupt, getter + 42, base + 0x904);
    assert(!Links(corrupt.data(), corrupt.size(), base, sites, owner, table));
    corrupt = image;
    Put(corrupt, getter + 32, base - 4);
    assert(!Links(corrupt.data(), corrupt.size(), base, sites, owner, table));
    unsigned count = 0;
    corrupt = image;
    corrupt[sites[0]] = 0x90;
    assert(FindUnique(corrupt.data(), corrupt.size(), Sites[0].bytes, count) == Missing && count == 0);
    const auto copy = Decode(Sites[0].bytes);
    std::copy_n(image.begin() + sites[0], copy.size(), image.begin() + 0xc00);
    assert(FindUnique(image.data(), image.size(), Sites[0].bytes, count) == Missing && count == 2);
}
struct Section { uint32_t at, size, flags; };
void CheckReal(char const* path)
{
    // Only reads the operator-selected file. No engine load, process launch, or DLL execution.
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open configured Game.dll");
    const std::vector<uint8_t> disk{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const size_t pe = Get(disk, 0x3c), optional = pe + 24;
    assert(Short(disk, 0) == 0x5a4d && Get(disk, pe) == 0x4550 &&
           Short(disk, pe + 4) == 0x14c && Short(disk, optional) == 0x10b);
    const uint32_t base = Get(disk, optional + 28), imageSize = Get(disk, optional + 56),
                   headers = Get(disk, optional + 60);
    assert(imageSize < 128u * 1024 * 1024 && Range(disk.size(), 0, headers) && Range(imageSize, 0, headers));
    std::vector<uint8_t> image(imageSize);
    std::copy_n(disk.begin(), headers, image.begin());
    std::vector<Section> sections;
    const size_t sectionTable = optional + Short(disk, pe + 20);
    for (unsigned i = 0; i < Short(disk, pe + 6); ++i) {
        const size_t section = sectionTable + 40 * i;
        const uint32_t virtualSize = Get(disk, section + 8), at = Get(disk, section + 12),
                       size = Get(disk, section + 16), raw = Get(disk, section + 20);
        assert(Range(disk.size(), raw, size) && Range(image.size(), at, size));
        std::copy_n(disk.begin() + raw, size, image.begin() + at);
        sections.push_back({at, virtualSize, Get(disk, section + 36)});
    }
    size_t sites[5]{};
    auto checkPatterns = [&] {
        for (size_t i = 0; i < 5; ++i) {
            unsigned count = 0;
            sites[i] = Missing;
            for (auto const& section : sections) {
                if (!(section.flags & 0x20000000)) continue;
                assert(Range(image.size(), section.at, section.size));
                unsigned found = 0;
                const size_t match = FindUnique(image.data() + section.at, section.size, Sites[i].bytes, found);
                if (match != Missing) sites[i] = section.at + match;
                count += found;
            }
            assert(count == 1 && sites[i] == Sites[i].hint);
        }
    };
    checkPatterns();
    const size_t exports = Get(image, optional + 96);
    for (size_t i = 2; i < 4; ++i) {
        unsigned found = 0;
        const uint32_t names = Get(image, exports + 32), ordinals = Get(image, exports + 36),
                       functions = Get(image, exports + 28);
        for (unsigned n = 0; n < Get(image, exports + 24); ++n) {
            const size_t name = Get(image, names + n * 4);
            const size_t length = std::strlen(Sites[i].symbol) + 1;
            if (Range(image.size(), name, length) &&
                !std::memcmp(image.data() + name, Sites[i].symbol, length)) {
                assert(Get(image, functions + Short(image, ordinals + n * 2) * 4) == sites[i]);
                ++found;
            }
        }
        assert(found == 1);
    }
    size_t owner = 0, table = 0;
    assert(Links(image.data(), image.size(), base, sites, owner, table));
    assert(owner == 0x403c20 && table == 0x2d9aa0);
    const size_t relocations = Get(image, optional + 136), end = relocations + Get(image, optional + 140);
    const uint32_t delta = 0x21000000;
    for (size_t block = relocations; block < end;) {
        const uint32_t page = Get(image, block), size = Get(image, block + 4);
        assert(size >= 8 && Range(end, block, size));
        for (size_t at = block + 8; at < block + size; at += 2) {
            const uint16_t entry = Short(image, at);
            if (entry >> 12 == 3) {
                const size_t target = page + (entry & 0xfff);
                Put(image, target, Get(image, target) + delta);
            } else assert(entry >> 12 == 0);
        }
        block += size;
    }
    assert(Links(image.data(), image.size(), base + delta, sites, owner, table));
    checkPatterns();
}
}
int main(int argc, char** argv)
{
    Fixture();
    if (argc == 2) CheckReal(argv[1]);
}
