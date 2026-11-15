// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "saving_policy.h"
#include "personal_store.h"
#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <limits>
#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace g3t::saving::store {
using Bytes = std::vector<uint8_t>;
inline constexpr uint8_t Magic[8] = {'G', '3', 'T', 'S', 'A', 'V', 'E', '\0'};
constexpr uint32_t Version = 4, MinReaderVersion = 4, Limit = 128u * 1024 * 1024;
constexpr size_t HeaderSize = 112, SectionHeaderSize = 28;
constexpr uint32_t SectionRequired = 0, SectionOptional = 1;
constexpr uint32_t CharacterLimit = 256u * 1024, ExtensionLimit = 1024u * 1024;
constexpr uint32_t NativeHeader = 1, NativeWorld = 2, Journal = 3, Character = 4, Personal = 5, Extension = 256;
constexpr uint32_t PlayerLimit = 20 + CharacterLimit + personal::Limit;
struct Block {
    uint32_t type = 0, version = 0;
    uint64_t account = 0;
    Bytes bytes;
    uint32_t flags = 0;
};
struct Save {
    Key key;
    std::vector<Block> blocks;
    uint32_t schemaVersion = Version, minReaderVersion = MinReaderVersion;
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

inline uint32_t CrcByte(uint32_t crc, uint8_t byte) { return saving::CrcByte(crc, byte); }
inline uint32_t Crc(uint8_t const* data, size_t size) { return saving::Crc32(data, size); }
inline uint32_t Crc(Bytes const& bytes) { return Crc(bytes.data(), bytes.size()); }
inline uint32_t LegacyContainerCrc(Bytes const& bytes)
{
    return Crc32Zeroed(bytes.data(), bytes.size(), 16, 4);
}
inline uint32_t ContainerCrc(Bytes const& bytes)
{
    return Crc32Zeroed(bytes.data(), bytes.size(), 24, 4);
}
inline void Put(Bytes& bytes, uint64_t value, unsigned count) { PutLe(bytes, value, count); }
inline bool Get(Bytes const& bytes, size_t& at, uint64_t& value, unsigned count)
{
    return GetLe(bytes.data(), bytes.size(), at, value, count);
}
inline bool HeroPose(Bytes const& bytes, Key& key, RecordPose& pose, uint32_t* outVersion = nullptr)
{
    if (bytes.empty() || bytes.size() > CharacterLimit) return false;
    if (IsHeroBinaryMagic(bytes.data(), bytes.size())) {
        return InspectHeroBinary(bytes.data(), bytes.size(), key, outVersion, nullptr, nullptr, &pose);
    }
    if (std::find(bytes.begin(), bytes.end(), uint8_t(0)) != bytes.end()) return false;
    std::istringstream input(std::string(bytes.begin(), bytes.end()));
    std::string lines[3];
    for (auto& line : lines) {
        if (!std::getline(input, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
    }
    if (!ParseRecordMetadata(lines[0], lines[1], lines[2], nullptr, key, pose)) return false;
    if (outVersion) *outVersion = 4;
    return true;
}
inline bool HeroKey(Bytes const& bytes, Key& key, uint32_t* outVersion = nullptr)
{
    RecordPose pose;
    return HeroPose(bytes, key, pose, outVersion);
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
    if (IsHeroBinaryMagic(bytes.data(), bytes.size())) return RebindHeroBinary(bytes, key);
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
inline bool ValidSave(Save const& save, uint32_t containerVersion = Version)
{
    if (!Valid(save.key) || save.blocks.size() > 64) return false;
    unsigned accounts = 0;
    uint64_t total = HeaderSize;
    for (size_t i = 0; i < save.blocks.size(); ++i) {
        auto const& b = save.blocks[i];
        total += SectionHeaderSize + b.bytes.size();
        if (total > Limit || !b.version) return false;
        for (size_t j = 0; j < i; ++j)
            if (save.blocks[j].type == b.type && save.blocks[j].account == b.account) return false;
        if (b.type == Character) {
            Key hero;
            uint32_t heroVer = 0;
            if (++accounts > 8 || !b.account || !HeroKey(b.bytes, hero, &heroVer) ||
                b.version != heroVer || heroVer < 4 ||
                !Exact(hero, {save.key.world, save.key.save, b.account})) return false;
        } else if (b.type == Personal) {
            personal::Snapshot snapshot;
            if (!b.account || b.version != 1 || !Find(save, Character, b.account) ||
                !personal::Decode(b.bytes, snapshot)) return false;
        } else if (b.type == NativeHeader || b.type == NativeWorld) {
            if (b.account || b.version != 1 || b.bytes.empty() || b.bytes.size() > 64u * 1024 * 1024) return false;
        } else if (b.type == Journal) {
            if (b.account || b.version != 1 || b.bytes.size() > ExtensionLimit) return false;
        } else if (b.type >= Extension) {
            if (b.account || b.bytes.size() > ExtensionLimit) return false;
        } else if ((b.flags & SectionOptional) != 0) {
            if (b.bytes.size() > ExtensionLimit) return false;
        } else {
            return false;
        }
    }
    return Find(save, NativeHeader) && Find(save, NativeWorld) &&
           (containerVersion == 2 || Find(save, Journal)) && Find(save, Character, save.key.player);
}
inline bool Encode(Save const& save, Bytes& out)
{
    if (!ValidSave(save, Version)) return false;
    Bytes payload;
    for (auto const& b : save.blocks) {
        Put(payload, b.type, 4); Put(payload, b.version, 4); Put(payload, b.flags, 4);
        Put(payload, b.account, 8); Put(payload, b.bytes.size(), 4); Put(payload, Crc(b.bytes), 4);
        payload.insert(payload.end(), b.bytes.begin(), b.bytes.end());
    }
    if (HeaderSize + payload.size() > Limit) return false;
    Bytes result(Magic, Magic + sizeof(Magic));
    Put(result, Version, 4); Put(result, MinReaderVersion, 4);
    Put(result, HeaderSize, 4); Put(result, payload.size(), 4);
    Put(result, 0, 4); // containerCrc placeholder [24..28)
    Put(result, 0, 4); // flags
    result.insert(result.end(), save.key.world.begin(), save.key.world.end());
    result.insert(result.end(), save.key.save.begin(), save.key.save.end());
    Put(result, save.key.player, 8); Put(result, save.blocks.size(), 4);
    const uint32_t headerCrc = Crc32Zeroed(result.data(), 108, 24, 4);
    Put(result, headerCrc, 4);
    result.insert(result.end(), payload.begin(), payload.end());
    const uint32_t checksum = ContainerCrc(result);
    PutLeAt(result.data() + 24, checksum, 4);
    out = std::move(result);
    return true;
}
inline bool Modern(Bytes const& bytes)
{
    // Recognize damaged/truncated modern headers too; never downgrade them to a native save.
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "G3TS", 4) == 0;
}
inline bool DecodeSections(Bytes const& bytes, size_t& at, uint64_t count, bool hasFlags, Save& result,
                           char const** reason)
{
    result.blocks.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        Block b;
        uint64_t type = 0, blockVersion = 0, secFlags = 0, size = 0, checksum = 0;
        if (!Get(bytes, at, type, 4) || !Get(bytes, at, blockVersion, 4) ||
            (hasFlags && !Get(bytes, at, secFlags, 4)) || !Get(bytes, at, b.account, 8) ||
            !Get(bytes, at, size, 4) || !Get(bytes, at, checksum, 4) || size > bytes.size() - at) {
            if (reason) *reason = "section-bounds";
            return false;
        }
        b.type = static_cast<uint32_t>(type);
        b.version = static_cast<uint32_t>(blockVersion);
        b.flags = static_cast<uint32_t>(secFlags);
        if (checksum != Crc(bytes.data() + at, static_cast<size_t>(size))) {
            if (reason) *reason = "section-crc";
            return false;
        }
        b.bytes.assign(bytes.begin() + at, bytes.begin() + at + static_cast<size_t>(size));
        at += static_cast<size_t>(size);
        result.blocks.push_back(std::move(b));
    }
    return true;
}
inline bool DecodeLegacyContainer(Bytes const& bytes, uint32_t expectedVersion, Save& out, char const** reason)
{
    if (bytes.size() < 96 || bytes.size() > Limit) { if (reason) *reason = "truncated"; return false; }
    size_t at = 8;
    uint64_t version = 0, length = 0, crc = 0, count = 0;
    if (!Get(bytes, at, version, 4) || version != expectedVersion || !Get(bytes, at, length, 4) ||
        !Get(bytes, at, crc, 4) || length != bytes.size() - 96 || crc != LegacyContainerCrc(bytes)) {
        if (reason) *reason = "header-or-crc";
        return false;
    }
    Save result;
    result.schemaVersion = expectedVersion;
    result.minReaderVersion = expectedVersion;
    result.key.world.assign(bytes.begin() + 20, bytes.begin() + 52);
    result.key.save.assign(bytes.begin() + 52, bytes.begin() + 84);
    at = 84;
    if (!Get(bytes, at, result.key.player, 8) || !Get(bytes, at, count, 4) || count > 64) {
        if (reason) *reason = "header";
        return false;
    }
    if (!DecodeSections(bytes, at, count, false, result, reason)) return false;
    if (at != bytes.size() || !ValidSave(result, expectedVersion)) {
        if (reason) *reason = "invalid-save";
        return false;
    }
    out = std::move(result);
    return true;
}
inline bool Decode(Bytes const& bytes, Save& out, char const** reason = nullptr)
{
    if (reason) *reason = nullptr;
    if (bytes.size() > Limit) { if (reason) *reason = "too-large"; return false; }
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "G3TSAVE3", 8) == 0) {
        return DecodeLegacyContainer(bytes, 3, out, reason);
    }
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "G3TSAVE2", 8) == 0) {
        return DecodeLegacyContainer(bytes, 2, out, reason);
    }
    if (bytes.size() < HeaderSize || std::memcmp(bytes.data(), Magic, sizeof(Magic)) != 0) {
        if (reason) *reason = "header";
        return false;
    }
    size_t at = 8;
    uint64_t schema = 0, minReader = 0, headerSize = 0, length = 0, crc = 0, flags = 0, count = 0, headerCrc = 0;
    if (!Get(bytes, at, schema, 4) || !Get(bytes, at, minReader, 4) ||
        !Get(bytes, at, headerSize, 4) || !Get(bytes, at, length, 4) ||
        !Get(bytes, at, crc, 4) || !Get(bytes, at, flags, 4)) {
        if (reason) *reason = "truncated";
        return false;
    }
    if (schema < Version || minReader == 0 || minReader > Version || minReader > schema) {
        if (reason) *reason = "unsupported-version";
        return false;
    }
    if (headerSize < HeaderSize || headerSize > bytes.size() || length != bytes.size() - headerSize) {
        if (reason) *reason = "header-bounds";
        return false;
    }
    Save result;
    result.schemaVersion = static_cast<uint32_t>(schema);
    result.minReaderVersion = static_cast<uint32_t>(minReader);
    result.key.world.assign(bytes.begin() + 32, bytes.begin() + 64);
    result.key.save.assign(bytes.begin() + 64, bytes.begin() + 96);
    at = 96;
    if (!Get(bytes, at, result.key.player, 8) || !Get(bytes, at, count, 4) ||
        !Get(bytes, at, headerCrc, 4) || count > 64) {
        if (reason) *reason = "header";
        return false;
    }
    if (headerCrc != Crc32Zeroed(bytes.data(), 108, 24, 4)) {
        if (reason) *reason = "header-crc";
        return false;
    }
    if (crc != ContainerCrc(bytes)) {
        if (reason) *reason = "container-crc";
        return false;
    }
    at = static_cast<size_t>(headerSize);
    if (!DecodeSections(bytes, at, count, true, result, reason)) return false;
    if (at != bytes.size() || !ValidSave(result, Version)) {
        if (reason) *reason = "invalid-save";
        return false;
    }
    out = std::move(result);
    return true;
}

// Production atomic writer shared with engine-free fault tests. The old destination is never opened for writing.
enum class WriteStage { Opened, Written, Flushed, Closed };
using ContinueWrite = bool (*)(WriteStage);
struct IoError {
    char const* operation = "";
    std::string path, destination;
    uint32_t code = 0;
};
inline uint32_t LastIoError()
{
#ifdef _WIN32
    return GetLastError();
#else
    return static_cast<uint32_t>(errno);
#endif
}
inline bool Fail(IoError* error, char const* operation, std::string const& path,
                 uint32_t code, std::string const& destination = {})
{
    if (error) *error = {operation, path, destination, code};
    return false;
}
inline bool Missing(uint32_t code)
{
#ifdef _WIN32
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
#else
    return code == ENOENT;
#endif
}
inline bool Busy(uint32_t code)
{
#ifdef _WIN32
    return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
#else
    return code == EBUSY || code == EAGAIN;
#endif
}
inline char const* IoReason(IoError const& error, char const* fallback)
{
#ifdef _WIN32
    if (error.code == ERROR_DISK_FULL || error.code == ERROR_HANDLE_DISK_FULL ||
        error.code == ERROR_NOT_ENOUGH_QUOTA) return "disk-full";
    if (error.code == ERROR_ACCESS_DENIED || error.code == ERROR_PRIVILEGE_NOT_HELD) return "save-access-denied";
#else
    if (error.code == ENOSPC || error.code == EDQUOT) return "disk-full";
    if (error.code == EACCES || error.code == EPERM) return "save-access-denied";
#endif
    return fallback;
}
inline bool Read(std::string const& path, Bytes& out, IoError* error = nullptr)
{
    if (error) *error = {};
    out.clear();
#ifdef _WIN32
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Fail(error, "read-open", path, GetLastError());
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != 0;
    if (!ok) Fail(error, "read-size", path, GetLastError());
    else if (size.QuadPart < 0 || size.QuadPart > Limit)
        ok = Fail(error, "read-limit", path, ERROR_FILE_TOO_LARGE);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!out.empty() && !ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr))
            ok = Fail(error, "read", path, GetLastError());
        else if (read != out.size()) ok = Fail(error, "read-short", path, ERROR_HANDLE_EOF);
    }
    if (!CloseHandle(file) && ok) ok = Fail(error, "read-close", path, GetLastError());
#else
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return Fail(error, "read-open", path, LastIoError());
    bool ok = std::fseek(file, 0, SEEK_END) == 0;
    const long size = ok ? std::ftell(file) : -1;
    if (!ok || size < 0) ok = Fail(error, "read-size", path, LastIoError());
    else if (static_cast<unsigned long>(size) > Limit) ok = Fail(error, "read-limit", path, EFBIG);
    else {
        out.resize(static_cast<size_t>(size));
        if (std::fseek(file, 0, SEEK_SET) || std::fread(out.data(), 1, out.size(), file) != out.size())
            ok = Fail(error, "read", path, LastIoError());
    }
    if (std::fclose(file) && ok) ok = Fail(error, "read-close", path, LastIoError());
#endif
    if (!ok) out.clear();
    return ok;
}
inline bool Remove(std::string const& path, IoError* error = nullptr)
{
#ifdef _WIN32
    if (DeleteFileA(path.c_str())) return true;
#else
    if (std::remove(path.c_str()) == 0) return true;
#endif
    const uint32_t code = LastIoError();
    return Missing(code) || Fail(error, "delete", path, code);
}
inline bool ReadNative(std::string const& path, Bytes& bytes, IoError& error, std::string& source)
{
    source = path;
    if (Read(path, bytes, &error)) return true;
    if (!Missing(error.code)) return false;
    const IoError missing = error;
    // SaveGameWorld closes both streams but does not check its final MoveFileA calls.
    source = path + ".tmp";
    if (Read(source, bytes, &error)) return true;
    if (Missing(error.code)) { error = missing; source = path; }
    return false;
}
inline char const* NativeReason(IoError const& error)
{
    return IoReason(error, Missing(error.code) ? "native-files-missing" :
                           Busy(error.code) ? "native-files-busy" :
#ifdef _WIN32
                           error.code == ERROR_HANDLE_EOF
#else
                           error.code == ENODATA
#endif
                           ? "native-files-empty" : "native-files-read");
}
inline bool RetryNative(IoError const& error, uint32_t elapsed)
{
    return elapsed < 3000 && (Missing(error.code) || Busy(error.code) ||
#ifdef _WIN32
                             error.code == ERROR_HANDLE_EOF
#else
                             error.code == ENODATA
#endif
                             );
}
inline bool Atomic(std::string const& path, Bytes const& bytes, ContinueWrite proceed = nullptr,
                   IoError* error = nullptr)
{
    if (path.empty() || bytes.size() > Limit) return Fail(error, "write-limit", path, 0);
    const std::string temp = path + ".tmp";
    auto next = [proceed](WriteStage stage) { return !proceed || proceed(stage); };
#ifdef _WIN32
    HANDLE file = CreateFileA(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Fail(error, "write-open", temp, GetLastError());
    DWORD written = 0;
    bool ok = next(WriteStage::Opened);
    if (!ok) Fail(error, "write-injected", temp, GetLastError());
    if (ok && !WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
        ok = Fail(error, "write", temp, GetLastError());
    if (ok && written != bytes.size()) ok = Fail(error, "write-short", temp, ERROR_WRITE_FAULT);
    if (ok && !next(WriteStage::Written)) ok = Fail(error, "write-injected", temp, GetLastError());
    if (ok && !FlushFileBuffers(file)) ok = Fail(error, "flush", temp, GetLastError());
    if (ok && !next(WriteStage::Flushed)) ok = Fail(error, "flush-injected", temp, GetLastError());
    if (!CloseHandle(file) && ok) ok = Fail(error, "write-close", temp, GetLastError());
    if (ok && !next(WriteStage::Closed)) ok = Fail(error, "close-injected", temp, GetLastError());
    if (ok && !MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ok = Fail(error, "rename", temp, GetLastError(), path);
    if (!ok) Remove(temp);
#else
    int file = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0) return Fail(error, "write-open", temp, LastIoError());
    bool ok = next(WriteStage::Opened);
    if (!ok) Fail(error, "write-injected", temp, LastIoError());
    size_t at = 0;
    while (ok && at < bytes.size()) {
        const ssize_t n = write(file, bytes.data() + at, bytes.size() - at);
        if (n <= 0) ok = Fail(error, "write", temp, LastIoError());
        else at += static_cast<size_t>(n);
    }
    if (ok && !next(WriteStage::Written)) ok = Fail(error, "write-injected", temp, LastIoError());
    if (ok && fsync(file)) ok = Fail(error, "flush", temp, LastIoError());
    if (ok && !next(WriteStage::Flushed)) ok = Fail(error, "flush-injected", temp, LastIoError());
    if (close(file) && ok) ok = Fail(error, "write-close", temp, LastIoError());
    if (ok && !next(WriteStage::Closed)) ok = Fail(error, "close-injected", temp, LastIoError());
    if (ok && std::rename(temp.c_str(), path.c_str())) ok = Fail(error, "rename", temp, LastIoError(), path);
    if (!ok) Remove(temp);
#endif
    return ok;
}

enum class PublishStage { Marker, World, Header, Container };
using ContinuePublish = bool (*)(PublishStage, IoError&);
inline bool Repair(std::string const (&paths)[2], Save const& save, IoError& error,
                   IoError& recovery, ContinuePublish proceed = nullptr)
{
    error = {}; recovery = {};
    if (!Find(save, NativeHeader) || !Find(save, NativeWorld))
        return Fail(&error, "cache-format", paths[0], 0);
    Bytes previous[2];
    bool present[2]{}, changed[2]{};
    for (int i = 0; i < 2; ++i) {
        present[i] = Read(paths[i], previous[i], &error);
        if (!present[i] && !Missing(error.code)) return false;
    }
    for (int i : {1, 0}) {
        auto const* block = Find(save, i ? NativeWorld : NativeHeader);
        if (!block) return Fail(&error, "cache-format", paths[i], 0);
        if (present[i] && previous[i] == block->bytes) continue;
        const auto stage = i ? PublishStage::World : PublishStage::Header;
        if ((proceed && !proceed(stage, error)) || !Atomic(paths[i], block->bytes, nullptr, &error)) {
            for (int j = 0; j < 2; ++j) {
                if (!changed[j]) continue;
                IoError rollback;
                if (!(present[j] ? Atomic(paths[j], previous[j], nullptr, &rollback) : Remove(paths[j], &rollback)))
                    recovery = std::move(rollback);
            }
            return false;
        }
        changed[i] = true;
    }
    error = {};
    return true;
}
inline bool Publish(std::string const (&paths)[3], Save const& save, Bytes const& encoded,
                    IoError& error, IoError& recovery, ContinuePublish proceed = nullptr)
{
    error = {}; recovery = {};
    Save checked;
    if (!Decode(encoded, checked) || !Exact(save.key, checked.key))
        return Fail(&error, "publish-format", paths[2], 0);
    auto const* header = Find(checked, NativeHeader);
    auto const* world = Find(checked, NativeWorld);
    Bytes previous[2];
    bool present[2]{};
    for (int i = 0; i < 2; ++i) {
        IoError read;
        present[i] = Read(paths[i], previous[i], &read);
        if (!present[i] && !Missing(read.code)) { error = std::move(read); return false; }
    }
    const std::string marker = paths[2] + ".publish";
    auto step = [&](PublishStage stage, std::string const& path, Bytes const& bytes) {
        return (!proceed || proceed(stage, error)) && Atomic(path, bytes, nullptr, &error);
    };
    if (!step(PublishStage::Marker, marker, {'G','3','T','P','U','B','1'})) return false;
    bool changed[2]{};
    bool ok = step(PublishStage::World, paths[1], world->bytes);
    changed[1] = ok;
    if (ok) { ok = step(PublishStage::Header, paths[0], header->bytes); changed[0] = ok; }
    if (ok) ok = step(PublishStage::Container, paths[2], encoded);
    if (!ok) {
        bool restored = true;
        for (int i = 0; i < 2; ++i) {
            if (!changed[i]) continue;
            IoError rollback;
            if (!(present[i] ? Atomic(paths[i], previous[i], nullptr, &rollback) : Remove(paths[i], &rollback))) {
                restored = false;
                recovery = std::move(rollback);
            }
        }
        if (restored && !Remove(marker, &recovery)) return false;
        return false;
    }
    // A leftover marker is safe: load repairs from the now-complete container.
    Remove(marker, &recovery);
    return true;
}
}
