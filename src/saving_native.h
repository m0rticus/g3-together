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
constexpr size_t Missing = static_cast<size_t>(-1);
constexpr size_t Stolen = 6; // push ebp / mov ebp,esp / and esp,-8; no relative operand

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
inline bool Range(size_t size, size_t at, size_t length) { return at <= size && length <= size - at; }
inline uint32_t Word(uint8_t const* data, size_t at)
{
    uint32_t value;
    std::memcpy(&value, data + at, sizeof(value));
    return value;
}
inline bool Calls(uint8_t const* image, size_t size, size_t at, size_t target)
{
    if (!Range(size, at, 5) || image[at] != 0xe8 || target >= size) return false;
    int32_t relative;
    std::memcpy(&relative, image + at + 1, sizeof(relative));
    return int64_t(at) + 5 + relative == int64_t(target);
}

// CP 1.75 HUD call edges and singleton operand relationships, checked after unique matching.
inline bool Links(uint8_t const* image, size_t size, uint32_t base, size_t const (&site)[5],
                  size_t& singleton, size_t& vtable)
{
    for (size_t at : site) if (!Range(size, at, 60)) return false;
    if (!Calls(image, size, site[0] + 0xb9, site[2]) ||
        !Calls(image, size, site[1] + 0x2b2, site[3])) return false;
    const size_t getter = site[4];
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
}
