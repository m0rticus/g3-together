// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "saving_policy.h"
#include "personal_store.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace g3t::saving::store {
using Bytes = std::vector<uint8_t>;
constexpr uint32_t Version = 3, Limit = 128u * 1024 * 1024;
constexpr uint32_t CharacterLimit = 256u * 1024, ExtensionLimit = 1024u * 1024;
constexpr uint32_t NativeHeader = 1, NativeWorld = 2, Journal = 3, Character = 4, Personal = 5, Extension = 256;
constexpr uint32_t PlayerLimit = 20 + CharacterLimit + personal::Limit;
struct Block {
    uint32_t type = 0, version = 0;
    uint64_t account = 0;
    Bytes bytes;
};
struct Save {
    Key key;
    std::vector<Block> blocks;
};
struct LegacyManifest {
    Key key;
    uint32_t size[2]{}, crc[2]{};
};
inline bool ParseLegacy(Bytes const& bytes, LegacyManifest& out)
{
    if (bytes.empty() || bytes.size() > ExtensionLimit) return false;
    std::istringstream input(std::string(bytes.begin(), bytes.end()));
    std::string line, tag, extra;
    if (!std::getline(input, line) || (line != "G3T 2" && line != "G3T 2\r") || !std::getline(input, line)) return false;
    LegacyManifest found;
    std::istringstream fields(line);
    if (!(fields >> tag >> found.key.world >> found.key.save >> found.key.player >>
          found.size[0] >> found.crc[0] >> found.size[1] >> found.crc[1]) || fields >> extra ||
        tag != "checkpoint" || !Valid(found.key) || !found.size[0] || !found.size[1] ||
        found.size[0] > 64u * 1024 * 1024 || found.size[1] > 64u * 1024 * 1024) return false;
    bool ended = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (ended && !line.empty()) return false;
        if (line.compare(0, 7, "legacy ") == 0 && !SafeName(line.substr(7).c_str())) return false;
        if (line.compare(0, 15, "checkpoint_end ") == 0) {
            if (ended || line.substr(15) != found.key.save) return false;
            ended = true;
        }
    }
    if (!ended || input.bad()) return false;
    out = std::move(found);
    return true;
}

inline uint32_t CrcByte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    return crc;
}
inline uint32_t Crc(uint8_t const* data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc = CrcByte(crc, data[i]);
    }
    return crc ^ 0xffffffffu;
}
inline uint32_t Crc(Bytes const& bytes) { return Crc(bytes.data(), bytes.size()); }
inline uint32_t ContainerCrc(Bytes const& bytes)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < bytes.size(); ++i) crc = CrcByte(crc, i >= 16 && i < 20 ? 0 : bytes[i]);
    return crc ^ 0xffffffffu;
}
inline void Put(Bytes& bytes, uint64_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
inline bool Get(Bytes const& bytes, size_t& at, uint64_t& value, unsigned count)
{
    if (at > bytes.size() || count > bytes.size() - at) return false;
    value = 0;
    for (unsigned i = 0; i < count; ++i) value |= uint64_t(bytes[at++]) << (8 * i);
    return true;
}
inline bool HeroKey(Bytes const& bytes, Key& key)
{
    if (bytes.empty() || bytes.size() > CharacterLimit ||
        std::find(bytes.begin(), bytes.end(), uint8_t(0)) != bytes.end()) return false;
    std::istringstream input(std::string(bytes.begin(), bytes.end()));
    std::string lines[3];
    for (auto& line : lines) {
        if (!std::getline(input, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
    }
    RecordPose pose;
    return ParseRecordMetadata(lines[0], lines[1], lines[2], nullptr, key, pose);
}
inline bool PackPlayer(Bytes const& hero, Bytes const& state, Bytes& out)
{
    Key key;
    personal::Snapshot snapshot;
    if (!HeroKey(hero, key) || !personal::Decode(state, snapshot)) return false;
    Bytes result{'G','3','T','P','L','Y','R','1'};
    Put(result, 1, 4); Put(result, hero.size(), 4); Put(result, state.size(), 4);
    result.insert(result.end(), hero.begin(), hero.end());
    result.insert(result.end(), state.begin(), state.end());
    if (result.size() > PlayerLimit) return false;
    out = std::move(result);
    return true;
}
inline bool UnpackPlayer(Bytes const& bytes, Bytes& hero, Bytes& state)
{
    if (bytes.size() < 20 || bytes.size() > PlayerLimit || std::memcmp(bytes.data(), "G3TPLYR1", 8)) return false;
    size_t at = 8;
    uint64_t version = 0, heroSize = 0, stateSize = 0;
    if (!Get(bytes, at, version, 4) || version != 1 || !Get(bytes, at, heroSize, 4) ||
        !Get(bytes, at, stateSize, 4) || !heroSize || heroSize > CharacterLimit || stateSize > personal::Limit ||
        heroSize + stateSize != bytes.size() - at) return false;
    Bytes foundHero(bytes.begin() + at, bytes.begin() + at + static_cast<size_t>(heroSize));
    Bytes foundState(bytes.begin() + at + static_cast<size_t>(heroSize), bytes.end());
    Key key;
    personal::Snapshot snapshot;
    if (!HeroKey(foundHero, key) || !personal::Decode(foundState, snapshot)) return false;
    hero = std::move(foundHero); state = std::move(foundState);
    return true;
}
inline bool Rebind(Bytes& bytes, Key const& key)
{
    Key previous;
    if (!Valid(key) || !HeroKey(bytes, previous) || !SameCharacter(previous, key)) return false;
    std::string text(bytes.begin(), bytes.end());
    const size_t begin = text.find('\n') + 1, end = text.find('\n', begin);
    if (!begin || end == std::string::npos) return false;
    text.replace(begin, end - begin, "identity " + key.world + " " + key.save + " " + std::to_string(key.player));
    bytes.assign(text.begin(), text.end());
    return bytes.size() <= CharacterLimit;
}
inline Block const* Find(Save const& save, uint32_t type, uint64_t account = 0)
{
    for (auto const& b : save.blocks) if (b.type == type && b.account == account) return &b;
    return nullptr;
}
inline void Set(Save& save, Block block)
{
    for (auto& b : save.blocks)
        if (b.type == block.type && b.account == block.account) { b = std::move(block); return; }
    save.blocks.push_back(std::move(block));
}
inline bool ValidSave(Save const& save)
{
    if (!Valid(save.key) || save.blocks.size() > 64) return false;
    unsigned accounts = 0;
    uint64_t total = 96;
    for (size_t i = 0; i < save.blocks.size(); ++i) {
        auto const& b = save.blocks[i];
        total += 24 + b.bytes.size();
        if (total > Limit || !b.version) return false;
        for (size_t j = 0; j < i; ++j)
            if (save.blocks[j].type == b.type && save.blocks[j].account == b.account) return false;
        if (b.type == Character) {
            Key hero;
            if (++accounts > 8 || b.version != 4 || !b.account || !HeroKey(b.bytes, hero) ||
                !Exact(hero, {save.key.world, save.key.save, b.account})) return false;
        } else if (b.type == Personal) {
            personal::Snapshot snapshot;
            if (!b.account || b.version != 1 || !Find(save, Character, b.account) ||
                !personal::Decode(b.bytes, snapshot)) return false;
        } else if (b.type == NativeHeader || b.type == NativeWorld) {
            if (b.account || b.version != 1 || b.bytes.empty() || b.bytes.size() > 64u * 1024 * 1024) return false;
        } else if (b.type == Journal) {
            if (b.account || b.version != 1 || b.bytes.size() > ExtensionLimit) return false;
        } else if (b.type < Extension || b.account || b.bytes.size() > ExtensionLimit) return false;
    }
    return Find(save, NativeHeader) && Find(save, NativeWorld) && Find(save, Journal) &&
           Find(save, Character, save.key.player);
}
inline bool Encode(Save const& save, Bytes& out)
{
    if (!ValidSave(save)) return false;
    Bytes payload;
    for (auto const& b : save.blocks) {
        Put(payload, b.type, 4); Put(payload, b.version, 4); Put(payload, b.account, 8);
        Put(payload, b.bytes.size(), 4); Put(payload, Crc(b.bytes), 4);
        payload.insert(payload.end(), b.bytes.begin(), b.bytes.end());
    }
    Bytes result{'G','3','T','S','A','V','E','3'};
    Put(result, Version, 4); Put(result, payload.size(), 4); Put(result, 0, 4);
    result.insert(result.end(), save.key.world.begin(), save.key.world.end());
    result.insert(result.end(), save.key.save.begin(), save.key.save.end());
    Put(result, save.key.player, 8); Put(result, save.blocks.size(), 4);
    result.insert(result.end(), payload.begin(), payload.end());
    const uint32_t checksum = ContainerCrc(result);
    for (unsigned i = 0; i < 4; ++i) result[16 + i] = static_cast<uint8_t>(checksum >> (8 * i));
    out = std::move(result);
    return true;
}
inline bool Modern(Bytes const& bytes)
{
    // Recognize damaged/truncated modern headers too; never downgrade them to a native save.
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "G3TS", 4) == 0;
}
inline bool Decode(Bytes const& bytes, Save& out)
{
    if (bytes.size() < 96 || bytes.size() > Limit || std::memcmp(bytes.data(), "G3TSAVE3", 8)) return false;
    size_t at = 8;
    uint64_t version = 0, length = 0, crc = 0, count = 0;
    if (!Get(bytes, at, version, 4) || version != Version || !Get(bytes, at, length, 4) ||
        !Get(bytes, at, crc, 4) || length != bytes.size() - 96 ||
        crc != ContainerCrc(bytes)) return false;
    Save result;
    result.key.world.assign(bytes.begin() + 20, bytes.begin() + 52);
    result.key.save.assign(bytes.begin() + 52, bytes.begin() + 84);
    at = 84;
    if (!Get(bytes, at, result.key.player, 8) || !Get(bytes, at, count, 4) || count > 64) return false;
    for (uint64_t i = 0; i < count; ++i) {
        Block b;
        uint64_t type = 0, blockVersion = 0, size = 0, checksum = 0;
        if (!Get(bytes, at, type, 4) || !Get(bytes, at, blockVersion, 4) || !Get(bytes, at, b.account, 8) ||
            !Get(bytes, at, size, 4) || !Get(bytes, at, checksum, 4) || size > bytes.size() - at) return false;
        b.type = static_cast<uint32_t>(type); b.version = static_cast<uint32_t>(blockVersion);
        if (checksum != Crc(bytes.data() + at, static_cast<size_t>(size))) return false;
        b.bytes.assign(bytes.begin() + at, bytes.begin() + at + static_cast<size_t>(size));
        at += static_cast<size_t>(size);
        result.blocks.push_back(std::move(b));
    }
    if (at != bytes.size() || !ValidSave(result)) return false;
    out = std::move(result);
    return true;
}

// Production atomic writer shared with engine-free fault tests. The old destination is never opened for writing.
enum class WriteStage { Opened, Written, Flushed, Closed };
using ContinueWrite = bool (*)(WriteStage);
inline bool Atomic(std::string const& path, Bytes const& bytes, ContinueWrite proceed = nullptr)
{
    if (path.empty() || bytes.size() > Limit) return false;
    const std::string temp = path + ".tmp";
    auto next = [proceed](WriteStage stage) { return !proceed || proceed(stage); };
#ifdef _WIN32
    HANDLE file = CreateFileA(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = next(WriteStage::Opened) && WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size() && next(WriteStage::Written) && FlushFileBuffers(file) && next(WriteStage::Flushed);
    ok = CloseHandle(file) != 0 && ok;
    ok = next(WriteStage::Closed) && ok;
    if (ok) ok = MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileA(temp.c_str());
#else
    int file = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0) return false;
    bool ok = next(WriteStage::Opened);
    size_t at = 0;
    while (ok && at < bytes.size()) {
        const ssize_t n = write(file, bytes.data() + at, bytes.size() - at);
        if (n <= 0) ok = false;
        else at += static_cast<size_t>(n);
    }
    ok = ok && next(WriteStage::Written) && fsync(file) == 0 && next(WriteStage::Flushed);
    ok = close(file) == 0 && ok;
    ok = next(WriteStage::Closed) && ok;
    if (ok) ok = std::rename(temp.c_str(), path.c_str()) == 0;
    if (!ok) std::remove(temp.c_str());
#endif
    return ok;
}
}
