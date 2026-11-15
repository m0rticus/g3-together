// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/personal_store.h"

#include <cassert>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

using namespace g3t::saving::personal;

static std::string id(char c) { return std::string(16, c); }
static std::string numbered(size_t n)
{
    const uint32_t value = static_cast<uint32_t>(n + 1);
    std::string key(16, 0);
    for (unsigned i = 0; i != 4; ++i) key[i] = static_cast<char>(value >> (8 * i));
    return key;
}
static void u32(Bytes& b, size_t at, uint32_t v)
{
    for (unsigned i = 0; i != 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
static void invalid(Bytes const& bytes)
{
    Snapshot output{{id('z'), {Kind::Chest, 0, {}}}};
    assert(!Decode(bytes, output));
    assert(output.size() == 1 && output.begin()->first == id('z'));
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--hook-sites") {
        for (auto const& site : native::Sites) {
            std::cout << site.module << '\t' << site.symbol << '\t' << std::hex << site.rva
                      << '\t' << std::dec << site.stolen << '\t';
            for (size_t i = 0; i < site.headSize; ++i)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << unsigned(site.head[i]);
            std::cout << '\n';
        }
        return 0;
    }
    assert(argc == 1);
    for (auto const& site : native::Sites) {
        assert(native::Find(site.module, site.symbol) == &site);
        assert(native::Match(site, site.rva, site.head.data(), site.headSize) == site.stolen);
        assert(!native::Match(site, site.rva + 1, site.head.data(), site.headSize));
        assert(!native::Match(site, site.rva, nullptr, site.headSize));
        assert(!native::Match(site, site.rva, site.head.data(), site.headSize - 1));
        for (size_t i = 0; i < site.headSize; ++i) {
            auto corrupt = site.head;
            corrupt[i] ^= 1;
            assert(!native::Match(site, site.rva, corrupt.data(), site.headSize));
        }
    }
    assert(!native::Find("missing.dll", native::Sites[0].symbol));
    assert(!native::Find("Game.dll", "missing"));

    // Notification callbacks only mark dirty. A completed transfer commits the final amount;
    // destruction with uncommitted notifications refuses capture instead of saving teardown empties.
    ChangeTracker tracker;
    int cached = 5, live = 5;
    tracker.Changed(); live = 4;
    assert(tracker.dirty && tracker.CanRefresh());
    if (tracker.CanRefresh()) { cached = live; tracker.Refreshed(); }
    assert(cached == 4 && !tracker.dirty);
    tracker.Destroyed();
    tracker.Changed();
    assert(tracker.destroyed && !tracker.uncertain && cached == 4);
    ChangeTracker teardown;
    live = 0;
    teardown.Changed();
    teardown.Destroyed();
    assert(teardown.uncertain && !teardown.CanRefresh());
    if (teardown.CanRefresh()) { cached = live; teardown.Refreshed(); }
    assert(cached == 4);
    ChangeTracker destroyedFirst;
    destroyedFirst.Destroyed();
    destroyedFirst.Changed();
    assert(!destroyedFirst.dirty && !destroyedFirst.uncertain && !destroyedFirst.CanRefresh());

    Snapshot empty;
    Bytes bytes{42};
    assert(Encode(empty, bytes) && bytes.size() == 12);
    Snapshot round{{id('x'), {Kind::Corpse, 0, {}}}};
    assert(Decode(bytes, round) && round.empty());
    Snapshot ground{{id('g'), {Kind::GroundTaken, 0, {}}}};
    assert(Encode(ground, bytes) && bytes.size() == 32 && bytes[28] == 4);
    assert(Decode(bytes, round) && round.at(id('g')).kind == Kind::GroundTaken);
    Bytes invalidGround = bytes;
    invalidGround[29] = GeneratedPlunder;
    invalid(invalidGround);
    invalidGround = bytes;
    invalidGround[30] = 1;
    invalidGround.insert(invalidGround.end(), 25, 0);
    std::fill(invalidGround.begin() + 32, invalidGround.begin() + 48, 'i');
    u32(invalidGround, 48, 1);
    invalid(invalidGround);
    ground[id('g')].flags = HasDoor;
    Bytes unchangedGround{9};
    assert(!Encode(ground, unchangedGround) && unchangedGround == Bytes({9}));
    ground[id('g')].flags = 0;
    ground[id('g')].stacks.push_back({id('i'), 1, 0, Stock::Normal});
    assert(!Encode(ground, unchangedGround) && unchangedGround == Bytes({9}));

    Snapshot input{
        {id('b'), {Kind::Trader, GeneratedTrade, {{id('c'), 5, 1023, Stock::Merchandise},
                                                  {id('a'), 1, 0, Stock::Normal}}}},
        {id('a'), {Kind::Chest, GeneratedPlunder | HasDoor | DoorOpen | HasLock | Locked, {}}},
        {id('c'), {Kind::Corpse, GeneratedPlunder, {{id('d'), 7, 42, Stock::Normal}}}}
    };
    assert(Encode(input, bytes));
    assert(bytes.size() == HeaderBytes + EncodedRowBytes(input.at(id('a'))) + EncodedRowBytes(input.at(id('b'))) +
                               EncodedRowBytes(input.at(id('c'))));
    assert(Decode(bytes, round) && round.size() == 3);
    assert(round.at(id('b')).stacks[0].item == id('a'));
    Bytes again;
    assert(Encode(round, again) && bytes == again);
    std::reverse(input.at(id('b')).stacks.begin(), input.at(id('b')).stacks.end());
    assert(Encode(input, again) && bytes == again);
    for (size_t n = 0; n < bytes.size(); ++n) invalid(Bytes(bytes.begin(), bytes.begin() + n));
    auto bad = bytes; bad.push_back(0); invalid(bad);
    bad = bytes; bad[0] = 'X'; invalid(bad);
    bad = bytes; u32(bad, 8, MaxRows + 1); invalid(bad);
    bad = bytes; std::fill(bad.begin() + 12, bad.begin() + 28, 0); invalid(bad);
    bad = bytes; bad[28] = 5; invalid(bad);
    bad = bytes; bad[29] = 0x80; invalid(bad);
    bad = bytes; bad[29] = DoorOpen; invalid(bad);
    bad = bytes; bad[29] = Locked; invalid(bad);
    bad = bytes; std::copy(bad.begin() + 12, bad.begin() + 28, bad.begin() + 32); invalid(bad);
    // The second row begins at 32; its first stack starts at 52.
    bad = bytes; bad[50] = 1; bad[51] = 2; invalid(bad);
    bad = bytes; std::fill(bad.begin() + 52, bad.begin() + 68, 0); invalid(bad);
    bad = bytes; u32(bad, 68, 0); invalid(bad);
    bad = bytes; u32(bad, 68, MaxAmount + 1); invalid(bad);
    bad = bytes; u32(bad, 72, MaxQuality + 1); invalid(bad);
    bad = bytes; bad[76] = 1; invalid(bad);
    // Claiming MaxStacks in a 32-byte row must fail cleanly without reading past the buffer end.
    Bytes claimedMaxStacks(HeaderBytes + RowHeaderBytes, 0);
    std::memcpy(claimedMaxStacks.data(), "G3TPERS1", 8);
    u32(claimedMaxStacks, 8, 1);
    std::fill(claimedMaxStacks.begin() + 12, claimedMaxStacks.begin() + 28, 'm');
    claimedMaxStacks[28] = static_cast<uint8_t>(Kind::Chest);
    claimedMaxStacks[30] = static_cast<uint8_t>(MaxStacks & 0xff);
    claimedMaxStacks[31] = static_cast<uint8_t>((MaxStacks >> 8) & 0xff);
    invalid(claimedMaxStacks);
    invalid(Bytes(Limit + 1, 0));
    Snapshot broken = input;
    broken.at(id('b')).stacks[0].amount = 0;
    Bytes unchanged{1, 2, 3};
    assert(!Encode(broken, unchanged) && unchanged == Bytes({1, 2, 3}));
    broken = input; broken[id('a')].stacks.resize(MaxStacks + 1);
    assert(!Encode(broken, unchanged) && unchanged == Bytes({1, 2, 3}));
    broken = input; broken[id('a')].kind = static_cast<Kind>(0);
    assert(!Encode(broken, unchanged) && unchanged == Bytes({1, 2, 3}));
    broken = input; broken[id('a')].flags = DoorOpen;
    assert(!Encode(broken, unchanged) && unchanged == Bytes({1, 2, 3}));
    broken = input; broken[id('b')].stacks[0].quality = MaxQuality + 1;
    assert(!Encode(broken, unchanged) && unchanged == Bytes({1, 2, 3}));
    broken = input; broken[id('b')].stacks[0].type = static_cast<Stock>(1);
    assert(!Encode(broken, unchanged) && unchanged == Bytes({1, 2, 3}));
    broken.clear();
    for (unsigned i = 1; i <= 82; ++i) {
        std::string key(16, 0); key[0] = static_cast<char>(i);
        Inventory row;
        row.stacks.resize(MaxStacks, {id('q'), 1, 0, Stock::Normal});
        broken.emplace(key, row);
    }
    assert(!Encode(broken, unchanged) && unchanged == Bytes({1, 2, 3}));
    broken.clear();
    for (size_t i = 0; i <= MaxInventoryRows; ++i) broken.emplace(numbered(i), Inventory{});
    assert(!Encode(broken, unchanged) && unchanged == Bytes({1, 2, 3}));

    Snapshot mixed;
    for (size_t i = 0; i < MaxInventoryRows; ++i) mixed.emplace(numbered(i), Inventory{});
    for (size_t i = MaxInventoryRows; i < MaxInventoryRows + 5000; ++i)
        mixed.emplace(numbered(i), Inventory{Kind::GroundTaken, 0, {}});
    assert(Encode(mixed, bytes) && Decode(bytes, round) && round.size() == MaxInventoryRows + 5000);
    assert(Encode(round, again) && bytes == again);
    mixed.emplace(numbered(MaxInventoryRows + 5000), Inventory{});
    assert(!Encode(mixed, unchanged) && unchanged == Bytes({1, 2, 3}));

    Snapshot tooManyInventories;
    for (size_t i = 0; i <= MaxInventoryRows; ++i)
        tooManyInventories.emplace(numbered(i), Inventory{Kind::GroundTaken, 0, {}});
    assert(Encode(tooManyInventories, bytes));
    for (size_t i = 0; i <= MaxInventoryRows; ++i) bytes[12 + i * 20 + 16] = static_cast<uint8_t>(Kind::Chest);
    invalid(bytes);

    Snapshot maximumGround;
    for (size_t i = 0; i < MaxRows; ++i)
        maximumGround.emplace(numbered(i), Inventory{Kind::GroundTaken, 0, {}});
    assert(Encode(maximumGround, bytes));
    assert(bytes.size() == 12 + MaxRows * 20 && bytes.size() <= Limit);
    assert(Decode(bytes, round) && round.size() == MaxRows);
    maximumGround.emplace(numbered(MaxRows), Inventory{Kind::GroundTaken, 0, {}});
    assert(!Encode(maximumGround, unchanged) && unchanged == Bytes({1, 2, 3}));
}
