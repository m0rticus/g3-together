// SPDX-License-Identifier: GPL-3.0-or-later
// File layer of the client rejoin template (policy: client_template.h). Production code shared with the engine-free fault
// tests: every operation keeps its originating system error and names the exact path, so a failure is explainable from the
// log alone. Only the three published files of the template slot and their ".tmp" staging files are ever touched.
#pragma once

#include "client_template.h"
#include "saving_store.h"

#include <sys/stat.h>
#include <string>

namespace g3t::rejoin::tmpl::store {
using Bytes = saving::store::Bytes;
using Error = saving::store::IoError;
// operation, path, system error, success: one call per attempted file operation (nullptr = silent).
using Trace = void (*)(char const* operation, std::string const& path, uint32_t code, bool ok);

struct Paths {
    std::string header, world, marker; // empty when the save folder is unavailable
    bool Usable() const { return !header.empty() && !world.empty() && !marker.empty(); }
};

inline File Probe(std::string const& path, bool probeLock = true)
{
    File file;
    if (path.empty()) { file.folderMissing = true; return file; }
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
        file.error = GetLastError();
        file.folderMissing = file.error == ERROR_PATH_NOT_FOUND;
        return file;
    }
    file.exists = true;
    file.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    file.stamp = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { file.size = 0; file.error = ERROR_DIRECTORY; return file; }
    if (!probeLock) return file;
    // Shared on every side, but asking for DELETE access: it fails exactly when another handle denies delete/rename
    // (the engine's .tmp writers open with read+write sharing only), which is what decides whether a file is settled.
    HANDLE open = CreateFileA(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (open != INVALID_HANDLE_VALUE) { CloseHandle(open); return file; }
    file.error = GetLastError();
    if (saving::store::Missing(file.error)) { file.exists = false; file.size = 0; }
    else file.locked = saving::store::Busy(file.error);
#else
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        file.error = static_cast<uint32_t>(errno);
        file.folderMissing = false;
        return file;
    }
    file.exists = true;
    file.size = S_ISDIR(info.st_mode) ? 0 : static_cast<uint64_t>(info.st_size);
    file.stamp = static_cast<uint64_t>(info.st_mtime);
    if (S_ISDIR(info.st_mode)) file.error = static_cast<uint32_t>(EISDIR);
    (void)probeLock;
#endif
    return file;
}
inline Facts Observe(Paths const& paths, bool probeLock = true)
{
    Facts facts;
    facts.header = Probe(paths.header, probeLock);
    facts.world = Probe(paths.world, probeLock);
    facts.marker = Probe(paths.marker, probeLock);
    facts.headerTmp = Probe(paths.header.empty() ? paths.header : paths.header + ".tmp", probeLock);
    facts.worldTmp = Probe(paths.world.empty() ? paths.world : paths.world + ".tmp", probeLock);
    facts.markerTmp = Probe(paths.marker.empty() ? paths.marker : paths.marker + ".tmp", probeLock);
    return facts;
}

inline Fail FailFor(Error const& error, Fail fallback)
{
    const char* reason = saving::store::IoReason(error, "");
    if (!std::strcmp(reason, "disk-full")) return Fail::DiskFull;
    if (!std::strcmp(reason, "save-access-denied")) return Fail::AccessDenied;
    if (error.code && saving::store::Busy(error.code)) return Fail::Busy;
    return fallback;
}

// Removes the template and every staging file, marker first (an interrupted removal can never leave a marker over a half
// template). Returns true only when nothing of it remains; otherwise `first` is the first failing operation.
inline bool RemoveOwn(Paths const& paths, Error& first, Trace trace = nullptr)
{
    first = {};
    if (!paths.Usable()) return saving::store::Fail(&first, "remove-folder", paths.header, 3 /*path not found*/);
    const std::string files[6] = {paths.marker, paths.header, paths.world,
                                  paths.marker + ".tmp", paths.header + ".tmp", paths.world + ".tmp"};
    bool clean = true;
    for (auto const& path : files) {
        if (!Probe(path, false).exists) continue;
        Error error;
        const bool ok = saving::store::Remove(path, &error);
        if (trace) trace("delete", path, ok ? 0 : error.code, ok);
        if (!ok) {
            clean = false;
            if (!first.code) first = error;
        }
    }
    for (auto const& path : files) {
        if (Probe(path, false).exists) {
            clean = false;
            if (!first.code) first = {"delete-remains", path, {}, 0};
        }
    }
    return clean;
}

// The engine ignores a failed final rename of its staging file. Move a finished staging file onto a final name that does not
// exist; never replace anything.
inline bool Recover(std::string const& staging, std::string const& destination, Error& error, Trace trace = nullptr)
{
    bool ok = false;
    uint32_t code = 0;
    if (Probe(destination, false).exists) {
#ifdef _WIN32
        code = ERROR_ALREADY_EXISTS;
#else
        code = static_cast<uint32_t>(EEXIST);
#endif
    } else {
#ifdef _WIN32
        ok = MoveFileExA(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) code = GetLastError();
#else
        ok = std::rename(staging.c_str(), destination.c_str()) == 0;
        if (!ok) code = static_cast<uint32_t>(errno);
#endif
    }
    if (trace) trace("recover-rename", staging, code, ok);
    return ok || saving::store::Fail(&error, "recover-rename", staging, code, destination);
}

// Both halves are complete: read them, record their sizes and CRCs and write the marker last. `why` names the failure.
inline bool SealFiles(Paths const& paths, Origin origin, Identity const& identity, Seal& out, Error& error, Fail& why,
                      saving::store::ContinueWrite inject = nullptr)
{
    Bytes header, world;
    if (!saving::store::Read(paths.header, header, &error)) { why = FailFor(error, Fail::NoHeader); return false; }
    if (!saving::store::Read(paths.world, world, &error)) { why = FailFor(error, Fail::NoWorld); return false; }
    if (header.empty()) { why = Fail::HeaderEmpty; return saving::store::Fail(&error, "seal-size", paths.header, 0); }
    if (world.empty()) { why = Fail::WorldEmpty; return saving::store::Fail(&error, "seal-size", paths.world, 0); }
    if (header.size() < MinHeaderBytes || header.size() > MaxHeaderBytes) {
        why = Fail::HeaderEmpty;
        return saving::store::Fail(&error, "seal-size", paths.header, static_cast<uint32_t>(header.size()));
    }
    if (world.size() < MinWorldBytes || world.size() > MaxWorldBytes) {
        why = Fail::WorldEmpty;
        return saving::store::Fail(&error, "seal-size", paths.world, static_cast<uint32_t>(world.size()));
    }
    if (HeaderStructure(header) != Why::None) {
        why = Fail::HeaderEmpty;
        return saving::store::Fail(&error, "seal-header-format", paths.header, 0);
    }
    if (WorldStructure(world) != Why::None) {
        why = Fail::WorldEmpty;
        return saving::store::Fail(&error, "seal-world-format", paths.world, 0);
    }
    Seal seal;
    seal.game = identity.game;
    seal.protocol = identity.protocol;
    seal.origin = origin;
    seal.headerSize = header.size();
    seal.worldSize = world.size();
    seal.headerCrc = saving::store::Crc(header);
    seal.worldCrc = saving::store::Crc(world);
    const std::string text = Encode(seal);
    if (!saving::store::Atomic(paths.marker, Bytes(text.begin(), text.end()), inject, &error)) {
        why = FailFor(error, Fail::Seal);
        return false;
    }
    out = seal;
    return true;
}

// What is on disk now. `deep` also reads both halves and checks the sealed CRCs (entry time, after a change).
inline Report Check(Paths const& paths, Identity const& identity, bool deep, Facts* observed = nullptr, Error* error = nullptr)
{
    const Facts facts = Observe(paths, false);
    if (observed) *observed = facts;
    Seal seal;
    Parse parsed = Parse::Empty;
    if (facts.marker.exists && facts.marker.size && facts.marker.size <= 4096) {
        Bytes bytes;
        Error local;
        if (saving::store::Read(paths.marker, bytes, &local)) parsed = Decode(bytes, seal);
        else { parsed = Parse::Format; if (error) *error = local; }
    } else if (facts.marker.exists && facts.marker.size) parsed = Parse::Format;
    Report report = Inspect(facts, parsed, seal, identity);
    if (!deep || report.health != Health::Valid) return report;
    Bytes header, world;
    Error local;
    if (!saving::store::Read(paths.header, header, &local) || !saving::store::Read(paths.world, world, &local)) {
        if (error) *error = local;
        report.health = Health::Corrupt;
        report.why = Why::CrcMismatch;
        return report;
    }
    if (const Why format = HeaderStructure(header); format != Why::None) { report.health = Health::Corrupt; report.why = format; return report; }
    if (const Why format = WorldStructure(world); format != Why::None) { report.health = Health::Corrupt; report.why = format; return report; }
    return Verify(report, saving::store::Crc(header), saving::store::Crc(world));
}
}
