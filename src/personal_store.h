// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace g3t::saving::personal {
using Bytes = std::vector<uint8_t>;

constexpr size_t Limit = 1024 * 1024;
constexpr size_t MaxRows = (Limit - 12) / 20;
constexpr size_t MaxInventoryRows = 4096;
constexpr size_t MaxStacks = 512;
constexpr uint32_t MaxAmount = 1000000;
constexpr uint32_t MaxQuality = 1023;

enum class Kind : uint8_t { Chest = 1, Corpse = 2, Trader = 3, GroundTaken = 4 };
enum class Stock : uint8_t { Normal = 0, Merchandise = 2 };
enum Flag : uint8_t {
    GeneratedPlunder = 1, GeneratedTrade = 2, HasDoor = 4, DoorOpen = 8, HasLock = 16, Locked = 32
};

struct Stack {
    std::string item;
    uint32_t amount = 0;
    uint32_t quality = 0;
    Stock type = Stock::Normal;
};
struct Inventory {
    Kind kind = Kind::Chest;
    uint8_t flags = 0;
    std::vector<Stack> stacks;
};
using Snapshot = std::map<std::string, Inventory>;

// Listener callbacks are not a completion boundary. Never commit from a callback or during teardown.
struct ChangeTracker {
    bool dirty = false;
    bool destroyed = false;
    bool uncertain = false;
    void Changed() { if (!destroyed) dirty = true; }
    void Destroyed() { if (dirty) uncertain = true; destroyed = true; }
    bool CanRefresh() const { return !destroyed && !uncertain; }
    void Refreshed() { if (CanRefresh()) dirty = false; }
};

namespace native {
// CP 1.75 export heads; stolen instructions were checked for complete, position-independent boundaries.
struct HookSite {
    char const* module;
    char const* symbol;
    uint32_t rva;
    std::array<uint8_t, 14> head;
    size_t headSize;
    size_t stolen;
};
inline constexpr HookSite Sites[] = {
    {"Game.dll", "?GeneratePlunderInvFromTreasureSets@gCInventory_PS@@QAE_NXZ", 0xb71f0,
     {0x51,0x56,0x8b,0xf1,0x80,0x7e,0x18,0x00,0x0f,0x85,0x36,0x01,0x00,0x00}, 14, 8},
    {"Game.dll", "?GenerateTradeInvFromTreasureSets@gCInventory_PS@@QAE_NXZ", 0xb7340,
     {0x51,0x56,0x8b,0xf1,0x80,0x7e,0x19,0x00,0x0f,0x85,0x9f,0x00,0x00,0x00}, 14, 8},
    {"Game.dll", "?RefreshTradeInvFromTreasureSets@gCInventory_PS@@QAE_NXZ", 0xb7400,
     {0x56,0x8b,0xf1,0x8b,0x06,0x8b,0x50,0x68,0xff,0xd2,0x85,0xc0}, 12, 5},
    {"Game.dll", "?BeginTrade@gCGUIManager@@QAE_NPAVeCEntity@@@Z", 0x7bfd0,
     {0x55,0x8b,0xec,0x83,0xe4,0xf8,0x51,0x53,0x56,0x57,0x8b,0xf9}, 12, 6},
    {"Game.dll", "?TransferItemsTo@gCInventory_PS@@QAE_NPAV1@HHW4gEStackType@@@Z", 0xb6e20,
     {0x83,0xec,0x0c,0x83,0x7c,0x24,0x10,0x00,0x53,0x55,0x56,0x57,0x8b,0xf9}, 14, 8},
    {"Game.dll", "?TransferStackTo@gCInventory_PS@@QAE_NPAV1@HW4gEStackType@@@Z", 0xb6de0,
     {0x8b,0x54,0x24,0x08,0x3b,0x51,0x34,0x73,0x23,0x8b,0x41,0x30}, 12, 7},
    {"Game.dll", "?TransferAllTo@gCInventory_PS@@QAE_NPAV1@W4gEStackType@@@Z", 0xb6d70,
     {0x51,0x83,0x7c,0x24,0x08,0x00,0x57,0x8b,0xf9,0x74,0x5a,0x55}, 12, 6},
    {"Game.dll", "?TransferTransactionItemsTo@gCInventory_PS@@IAE_NPAV1@W4gEStackType@@@Z", 0xb7170,
     {0x53,0x8b,0x5c,0x24,0x08,0x85,0xdb,0x57,0x8b,0xf9,0x74,0x6b}, 12, 5},
    {"Game.dll", "?PerformTransaction@gCInventory_PS@@QAE_NPAV1@W4gEStackType@@1@Z", 0xb7130,
     {0x56,0x57,0x8b,0x7c,0x24,0x0c,0x85,0xff,0x8b,0xf1,0x74,0x1f}, 12, 6},
    {"Script.dll", "?TransferAmountTo@PSInventory@@QAE_NHHAAVEntity@@@Z", 0x64d0,
     {0x55,0x8b,0xec,0x83,0xe4,0xf8,0x81,0xec,0xa8,0x00,0x00,0x00}, 12, 6},
};

inline HookSite const* Find(char const* module, char const* symbol)
{
    if (!module || !symbol) return nullptr;
    for (auto const& site : Sites)
        if (!std::strcmp(site.module, module) && !std::strcmp(site.symbol, symbol)) return &site;
    return nullptr;
}

inline size_t Match(HookSite const& site, uint32_t rva, uint8_t const* code, size_t size)
{
    if (!code || rva != site.rva || site.headSize > site.head.size() || size < site.headSize ||
        site.stolen < 5 || site.stolen > site.headSize ||
        !std::equal(site.head.begin(), site.head.begin() + site.headSize, code)) return 0;
    return site.stolen;
}
}

inline bool Id(std::string const& id)
{
    return id.size() == 16 && std::any_of(id.begin(), id.end(), [](char c) { return c != 0; });
}

inline bool Valid(Inventory const& row)
{
    if (row.kind == Kind::GroundTaken) return row.flags == 0 && row.stacks.empty();
    if (row.kind != Kind::Chest && row.kind != Kind::Corpse && row.kind != Kind::Trader) return false;
    if ((row.flags & 0xc0) || ((row.flags & DoorOpen) && !(row.flags & HasDoor)) ||
        ((row.flags & Locked) && !(row.flags & HasLock)) || row.stacks.size() > MaxStacks) return false;
    for (auto const& stack : row.stacks)
        if (!Id(stack.item) || !stack.amount || stack.amount > MaxAmount || stack.quality > MaxQuality ||
            (stack.type != Stock::Normal && stack.type != Stock::Merchandise)) return false;
    return true;
}

inline void Put(Bytes& out, uint32_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

inline bool Encode(Snapshot const& snapshot, Bytes& output)
{
    if (snapshot.size() > MaxRows) return false;
    size_t size = 12;
    size_t inventories = 0;
    for (auto const& entry : snapshot) {
        if (!Id(entry.first) || !Valid(entry.second)) return false;
        if (entry.second.kind != Kind::GroundTaken && ++inventories > MaxInventoryRows) return false;
        size += 20 + 25 * entry.second.stacks.size();
        if (size > Limit) return false;
    }
    Bytes out;
    out.reserve(size);
    static constexpr uint8_t magic[] = {'G','3','T','P','E','R','S','1'};
    out.insert(out.end(), magic, magic + sizeof(magic));
    Put(out, static_cast<uint32_t>(snapshot.size()), 4);
    for (auto const& [id, row] : snapshot) {
        out.insert(out.end(), id.begin(), id.end());
        out.push_back(static_cast<uint8_t>(row.kind));
        out.push_back(row.flags);
        Put(out, static_cast<uint32_t>(row.stacks.size()), 2);
        auto stacks = row.stacks;
        std::sort(stacks.begin(), stacks.end(), [](Stack const& a, Stack const& b) {
            if (a.item != b.item) return a.item < b.item;
            if (a.quality != b.quality) return a.quality < b.quality;
            if (a.type != b.type) return static_cast<uint8_t>(a.type) < static_cast<uint8_t>(b.type);
            return a.amount < b.amount;
        });
        for (auto const& stack : stacks) {
            out.insert(out.end(), stack.item.begin(), stack.item.end());
            Put(out, stack.amount, 4);
            Put(out, stack.quality, 4);
            out.push_back(static_cast<uint8_t>(stack.type));
        }
    }
    output.swap(out);
    return true;
}

inline bool Decode(Bytes const& bytes, Snapshot& output)
{
    static constexpr uint8_t magic[] = {'G','3','T','P','E','R','S','1'};
    if (bytes.size() < 12 || bytes.size() > Limit || !std::equal(magic, magic + 8, bytes.begin())) return false;
    size_t pos = 8;
    auto read = [&](unsigned count) {
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) value |= static_cast<uint32_t>(bytes[pos++]) << (8 * i);
        return value;
    };
    const uint32_t count = read(4);
    if (count > MaxRows || count > (bytes.size() - pos) / 20) return false;
    Snapshot result;
    size_t inventories = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes.size() - pos < 20) return false;
        std::string id(reinterpret_cast<char const*>(bytes.data() + pos), 16);
        pos += 16;
        Inventory row;
        row.kind = static_cast<Kind>(read(1));
        row.flags = static_cast<uint8_t>(read(1));
        const uint32_t stacks = read(2);
        if (stacks > MaxStacks || stacks > (bytes.size() - pos) / 25) return false;
        row.stacks.reserve(stacks);
        for (uint32_t j = 0; j < stacks; ++j) {
            Stack stack;
            stack.item.assign(reinterpret_cast<char const*>(bytes.data() + pos), 16);
            pos += 16;
            stack.amount = read(4);
            stack.quality = read(4);
            stack.type = static_cast<Stock>(read(1));
            row.stacks.push_back(std::move(stack));
        }
        if (!Id(id) || !Valid(row)) return false;
        if (row.kind != Kind::GroundTaken && ++inventories > MaxInventoryRows) return false;
        if (!result.emplace(std::move(id), std::move(row)).second) return false;
    }
    if (pos != bytes.size()) return false;
    output.swap(result);
    return true;
}
}
