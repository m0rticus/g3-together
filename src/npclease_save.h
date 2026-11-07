// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace g3t::npclease {
inline constexpr char SavePrefix[] = "npclease1 ";
inline constexpr size_t MaxOverlayText = 2 * (20 + MaxLeases * detail::SavedWireSize) + sizeof(SavePrefix);

struct RestoreEpochBinding {
    uint32_t expected = 0;

    void Bind(uint32_t current, uint32_t restored) { expected = restored != current ? restored : 0; }
    bool Consume(uint32_t current)
    {
        if (!expected || current != expected) return false;
        expected = 0;
        return true;
    }
};
inline bool EncodeSaveText(SaveOverlay const& overlay, std::string& output)
{
    std::vector<uint8_t> bytes;
    if (!EncodeOverlay(overlay, bytes)) return false;
    static constexpr char hex[] = "0123456789abcdef";
    std::string text = SavePrefix;
    text.reserve(sizeof(SavePrefix) + bytes.size() * 2);
    for (const auto byte : bytes) {
        text.push_back(hex[byte >> 4]);
        text.push_back(hex[byte & 15]);
    }
    text.push_back('\n');
    output.swap(text);
    return true;
}

inline bool DecodeSaveText(std::string const& text, SaveOverlay& output)
{
    constexpr size_t prefix = sizeof(SavePrefix) - 1;
    if (text.size() > MaxOverlayText || text.size() < prefix + 41 ||
        text.compare(0, prefix, SavePrefix) || text.back() != '\n' ||
        (text.size() - prefix - 1) % 2) return false;
    auto digit = [](char ch) -> int {
        return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
    };
    std::vector<uint8_t> bytes;
    bytes.reserve((text.size() - prefix - 1) / 2);
    for (size_t i = prefix; i + 1 < text.size() - 1; i += 2) {
        const int high = digit(text[i]), low = digit(text[i + 1]);
        if (high < 0 || low < 0) return false;
        bytes.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return DecodeOverlay(bytes.data(), bytes.size(), output);
}

inline bool ReadSaveStream(FILE* file, SaveOverlay& output)
{
    if (!file) return false;
    SaveOverlay decoded;
    bool first = true, matched = false, found = false;
    std::string text;
    char chunk[512];
    while (fgets(chunk, sizeof(chunk), file)) {
        const size_t count = std::strlen(chunk);
        if (first) {
            matched = !std::strncmp(chunk, SavePrefix, sizeof(SavePrefix) - 1);
            text.clear();
        }
        if (matched) {
            if (count > MaxOverlayText - text.size()) return false;
            text.append(chunk, count);
        }
        first = count && chunk[count - 1] == '\n';
        if (first && matched) {
            if (found || !DecodeSaveText(text, decoded)) return false;
            found = true;
        }
    }
    if (ferror(file) || !first || !found) return false;
    output.swap(decoded);
    return true;
}
}
