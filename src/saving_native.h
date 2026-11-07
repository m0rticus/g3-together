// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace g3t::saving::native {
struct Site { char const* name; uint32_t hint; char const* bytes; char const* symbol; };
inline constexpr Site Sites[] = {
    {"SaveGameFn", 0xa9e90, "558bec83e4f883ec1c535657e8????????8bf833db3bfb0f84", nullptr},
    {"LoadGameFn", 0xa9a10, "558bec83e4f88b450883ec5c83780400535657752f", nullptr},
    {"WorldSaveGameFn", 0x19c6b0, "558bec83e4f881ecb4000000538b5d0856894c2418578bcb",
     "?SaveGameWorld@gCWorld@@QAE_NABVbCString@@PAVeCGfxImageData@@PAXK@Z"},
    {"WorldLoadGameFn", 0x19d170, "558bec83e4f881ecbc000000538b5d0856578bf98d87800000005053",
     "?LoadGameWorld@gCWorld@@QAE_NABVbCString@@_N@Z"},
    {"FileManagerGetter", 0xa92d0,
     "b8010000008405????????75290905????????33c068????????c705????????????????a3????????a3????????e8????????83c404b8????????c3",
     nullptr}
};
inline constexpr Site UiSites[] = {
    {"GUI.HandleMessage", 0x7d9e0, "8b44240483ec682d010100005356578bf90f84",
     "?HandleMessage@gCGUIManager@@QAE_NIIH@Z"},
    {"HUD.NewSaveSlot", 0xa9e40, "5168????????8d4c2404ff15????????8d042450e8????????84c08d0c24741f", nullptr},
    {"Menu.NewSaveSlotCall", 0x8e97a, "e8????????50e8????????84c0755768????????8d4c240cff15????????", nullptr},
    {"Session.IsSaveAllowed", 0x1743a0,
     "568bf1e8????????85c0742e8b484085c9750433c0eb238b018b90ec0000006a0068????????68????????6a006a37ffd250e8????????83c41480781401750432c05ec3837e28008b461c75f350e8????????84c075e98b4e1c85c974198b018b90ec0000006a3cffd285c0740980b8a40000000174c9b0015ec3",
     "?IsSaveAllowed@gCSession@@QBE_NXZ"},
    {"HUD.CaptureSaveImage", 0xa9310,
     "e8????????85c07423578bc8e8????????8b4e0885c98bf8740e8b018b5024ffd2c7460800000000897e085fc3", nullptr}
};
constexpr size_t GuiStolen = 7;
inline constexpr Site DeleteSite = {
    "World.DeleteSaveGame", 0x19c0d0,
    "83ec105356578b7c242068????????8d44241050578d4c241c518d54242452e8????????",
    "?DeleteSaveGame@gCWorld@@QAE_NABVbCString@@@Z"
};
constexpr size_t Missing = static_cast<size_t>(-1);
constexpr size_t Stolen = 6; // push ebp / mov ebp,esp / and esp,-8; no relative operand
constexpr uint32_t PupReceiverRva = 0x157f0, PupImageSize = 0xa8000;
inline constexpr char PupReceiverBytes[] =
    "558bec6affff35????????ff15????????6a006a01ff35????????ff15????????a1????????5dffe0";

inline int Hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
inline std::vector<int> Decode(char const* pattern)
{
    std::vector<int> bytes;
    if (!pattern) return bytes;
    for (size_t i = 0; pattern[i]; i += 2) {
        if (!pattern[i + 1]) return {};
        if (pattern[i] == '?' && pattern[i + 1] == '?') bytes.push_back(-1);
        else {
            const int high = Hex(pattern[i]), low = Hex(pattern[i + 1]);
            if (high < 0 || low < 0) return {};
            bytes.push_back(high * 16 + low);
        }
    }
    return bytes;
}
inline size_t FindUnique(uint8_t const* data, size_t size, char const* pattern, unsigned& matches)
{
    matches = 0;
    const auto bytes = Decode(pattern);
    if (!data || bytes.empty() || bytes.size() > size) return Missing;
    size_t found = Missing;
    for (size_t at = 0; at <= size - bytes.size(); ++at) {
        size_t i = 0;
        while (i < bytes.size() && (bytes[i] < 0 || data[at + i] == bytes[i])) ++i;
        if (i == bytes.size()) { found = at; ++matches; }
    }
    return matches == 1 ? found : Missing;
}
inline bool Matches(uint8_t const* data, size_t size, char const* pattern)
{
    const auto bytes = Decode(pattern);
    if (!data || bytes.empty() || bytes.size() > size) return false;
    for (size_t i = 0; i < bytes.size(); ++i)
        if (bytes[i] >= 0 && data[i] != bytes[i]) return false;
    return true;
}
inline bool Range(size_t size, size_t at, size_t length) { return at <= size && length <= size - at; }
inline uint32_t Word(uint8_t const* data, size_t at)
{
    uint32_t value;
    std::memcpy(&value, data + at, sizeof(value));
    return value;
}
inline bool BranchTarget(uint8_t const* data, size_t size, uint32_t address, uint8_t opcode, uint32_t& target)
{
    if (!data || size < 5 || data[0] != opcode || (opcode != 0xe8 && opcode != 0xe9)) return false;
    target = address + 5u + Word(data, 1);
    return true;
}
inline bool PupEntryTarget(uint8_t const* data, size_t size, uint32_t address, uint32_t& target)
{
    return Matches(data, size, "e9????????90") && BranchTarget(data, size, address, 0xe9, target);
}
inline bool PupShimTarget(uint8_t const* data, size_t size, uint32_t address, uint32_t& target)
{
    return Matches(data, size, "870c2451e9????????") && BranchTarget(data + 4, size - 4, address + 4, 0xe9, target);
}
inline bool PupReceiver(uint8_t const* data, size_t size, uint32_t base)
{
    return Matches(data, size, PupReceiverBytes) &&
           Word(data, 7) == base + 0x99510 && Word(data, 13) == base + 0x677a0 &&
           Word(data, 23) == base + 0x99510 && Word(data, 29) == base + 0x677a4 &&
           Word(data, 34) == base + 0x97578;
}
inline bool PupReceiverAt(uint8_t const* data, size_t size, uint32_t address, uint32_t base, uint32_t imageSize)
{
    return base && imageSize == PupImageSize && address >= base && address - base == PupReceiverRva &&
           PupReceiver(data, size, base);
}
inline bool Calls(uint8_t const* image, size_t size, size_t at, size_t target)
{
    if (!Range(size, at, 5) || image[at] != 0xe8 || target >= size) return false;
    int32_t relative;
    std::memcpy(&relative, image + at + 1, sizeof(relative));
    return int64_t(at) + 5 + relative == int64_t(target);
}

// CP 1.75 HUD call edges and singleton operand relationships, checked after unique matching.
inline bool GetterLinks(uint8_t const* image, size_t size, uint32_t base, size_t getter,
                        size_t& singleton, size_t& vtable)
{
    if (!Range(size, getter, 60)) return false;
    const uint32_t owner = Word(image, getter + 28), table = Word(image, getter + 32),
                   flag = Word(image, getter + 7);
    if (Word(image, getter + 15) != flag || Word(image, getter + 37) != owner + 4 ||
        Word(image, getter + 42) != owner + 8 || Word(image, getter + 55) != owner ||
        flag < base || owner < base || table < base ||
        !Range(size, flag - base, 4) || !Range(size, owner - base, 12) ||
        !Range(size, table - base, 4)) return false;
    const uint32_t firstVirtual = Word(image, table - base);
    if (firstVirtual < base || !Range(size, firstVirtual - base, 1)) return false;
    singleton = owner - base;
    vtable = table - base;
    return true;
}
inline bool Links(uint8_t const* image, size_t size, uint32_t base, size_t const (&site)[5],
                  size_t& singleton, size_t& vtable)
{
    for (size_t at : site) if (!Range(size, at, 60)) return false;
    return Calls(image, size, site[0] + 0xb9, site[2]) &&
           Calls(image, size, site[1] + 0x2b2, site[3]) &&
           GetterLinks(image, size, base, site[4], singleton, vtable);
}
}
