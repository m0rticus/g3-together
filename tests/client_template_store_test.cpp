// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/client_template_store.h"
#include "client_template_fixture.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace tmpl = g3t::rejoin::tmpl;
namespace store = g3t::rejoin::tmpl::store;
namespace io = g3t::saving::store;
namespace fs = std::filesystem;

namespace {
struct Event {
    std::string operation, path;
    uint32_t code;
    bool ok;
};
std::vector<Event> events;
void Trace(char const* operation, std::string const& path, uint32_t code, bool ok)
{
    events.push_back({operation, path, code, ok});
}

io::WriteStage failStage;
store::Paths observing;
std::vector<io::WriteStage> stages;
bool Inject(io::WriteStage stage)
{
    assert(fs::exists(observing.header) && fs::exists(observing.world));
    assert(!fs::exists(observing.marker));
    stages.push_back(stage);
    if (stage != failStage) return true;
#ifdef _WIN32
    SetLastError(ERROR_DISK_FULL);
#else
    errno = ENOSPC;
#endif
    return false;
}
bool ObserveWrite(io::WriteStage stage)
{
    assert(fs::exists(observing.header) && fs::exists(observing.world));
    assert(!fs::exists(observing.marker));
    stages.push_back(stage);
    return true;
}

void Write(std::string const& path, io::Bytes const& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    assert(file);
    file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(file.good());
    file.close();
    assert(file.good());
}
void WriteText(std::string const& path, std::string const& text)
{
    Write(path, io::Bytes(text.begin(), text.end()));
}
io::Bytes Read(std::string const& path)
{
    io::Bytes bytes;
    io::IoError error;
    assert(io::Read(path, bytes, &error));
    return bytes;
}
store::Paths Paths(fs::path const& dir, bool alternative = false)
{
    const std::string base = (dir / tmpl::Slot).string();
    return {base + (alternative ? ".g3savcpx" : ".g3savcp"),
            base + (alternative ? ".g3savcpxdat" : ".g3savcpdat"), base + ".g3t"};
}
void Expect(tmpl::Report const& r, tmpl::Health health, tmpl::Why why)
{
    assert(r.health == health && r.why == why);
}
void CheckClean(store::Paths const& paths)
{
    for (auto const& path : {paths.marker, paths.header, paths.world})
        assert(!fs::exists(path) && !fs::exists(path + ".tmp"));
}
void Clean(store::Paths const& paths)
{
    io::IoError error;
    assert(store::RemoveOwn(paths, error));
    CheckClean(paths);
}
void Seal(store::Paths const& paths, tmpl::Identity const& id, tmpl::Seal& seal,
          io::ContinueWrite inject = nullptr)
{
    io::IoError error;
    tmpl::Fail why = tmpl::Fail::None;
    assert(store::SealFiles(paths, tmpl::Origin::NewGame, id, seal, error, why, inject));
}
void Halves(store::Paths const& paths, io::Bytes const& header, io::Bytes const& world)
{
    Write(paths.header, header);
    Write(paths.world, world);
}
void Decoys(std::string const& quick, std::string const& world, io::Bytes const& content)
{
    assert(Read(quick) == content && Read(world) == content);
}

void Happy(store::Paths const& paths, tmpl::Identity const& id, io::Bytes const& header, io::Bytes const& world)
{
    CheckClean(paths);
    Halves(paths, header, world);
    tmpl::Seal seal;
    observing = paths;
    stages.clear();
    Seal(paths, id, seal, ObserveWrite);
    assert((stages == std::vector<io::WriteStage>{io::WriteStage::Opened, io::WriteStage::Written,
                                                  io::WriteStage::Flushed, io::WriteStage::Closed}));
    assert(seal.headerSize == header.size() && seal.worldSize == world.size());
    assert(seal.headerCrc == io::Crc(header) && seal.worldCrc == io::Crc(world));
    tmpl::Facts facts;
    Expect(store::Check(paths, id, false, &facts), tmpl::Health::Valid, tmpl::Why::None);
    const uint64_t signature = tmpl::Signature(facts);
    Expect(store::Check(paths, id, true, &facts), tmpl::Health::Valid, tmpl::Why::None);
    assert(tmpl::Signature(facts) == signature);
    Clean(paths);
}

void MissingHeader(store::Paths const& paths, tmpl::Identity const& id, io::Bytes const& world)
{
    Write(paths.world, world);
    tmpl::Seal seal;
    io::IoError error;
    tmpl::Fail why = tmpl::Fail::None;
    assert(!store::SealFiles(paths, tmpl::Origin::World, id, seal, error, why));
    assert(why == tmpl::Fail::NoHeader && !std::strcmp(error.operation, "read-open") && error.path == paths.header);
#ifdef _WIN32
    assert(error.code == ERROR_FILE_NOT_FOUND);
#else
    assert(error.code == ENOENT);
#endif
    Clean(paths);
}

void Recovery(store::Paths const& paths, io::Bytes const& world)
{
    const auto temp = paths.world + ".tmp";
    Write(temp, world);
    io::IoError error;
    events.clear();
    assert(store::Recover(temp, paths.world, error, Trace));
    assert(events.size() == 1 && events[0].operation == "recover-rename" &&
           events[0].path == temp && events[0].ok && !events[0].code);
    assert(Read(paths.world) == world && !fs::exists(temp));
    const io::Bytes other{1, 2, 3};
    Write(temp, other);
    assert(!store::Recover(temp, paths.world, error, Trace));
    assert(!std::strcmp(error.operation, "recover-rename") && error.path == temp && error.destination == paths.world);
#ifdef _WIN32
    assert(error.code == ERROR_ALREADY_EXISTS);
#else
    assert(error.code == EEXIST);
#endif
    assert(events.size() == 2 && events[1].operation == "recover-rename" &&
           events[1].path == temp && !events[1].ok && events[1].code == error.code);
    assert(Read(paths.world) == world && Read(temp) == other);
    Clean(paths);
}

#ifdef _WIN32
void RealAsync(store::Paths const& paths, tmpl::Identity const& id,
               io::Bytes const& header, io::Bytes const& world)
{
    Write(paths.header, header);
    const std::string temp = paths.world + ".tmp";
    Write(temp, world);
    HANDLE writer = CreateFileA(temp.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(writer != INVALID_HANDLE_VALUE);
    tmpl::Facts facts = store::Observe(paths);
    assert(!facts.world.exists && facts.worldTmp.exists && facts.worldTmp.locked);
    assert(facts.worldTmp.error == ERROR_SHARING_VIOLATION);
    tmpl::Publish publish;
    publish.Begin(tmpl::Origin::NewGame, 1000);
    publish.NativeReturned(true, 1, true, 1000);
    assert(publish.Poll(facts, 1300) == tmpl::Step::Wait && publish.Active());
    assert(CloseHandle(writer));
    assert(MoveFileA(temp.c_str(), paths.world.c_str()));
    assert(publish.Poll(store::Observe(paths), 1350) == tmpl::Step::Seal);
    tmpl::Seal seal;
    Seal(paths, id, seal);
    publish.Finish(true);
    assert(publish.failure() == tmpl::Fail::None && !publish.Active());
    Expect(store::Check(paths, id, true), tmpl::Health::Valid, tmpl::Why::None);
    Clean(paths);
}

void SharedWriter(store::Paths const& paths, io::Bytes const& header, io::Bytes const& world)
{
    const std::string temp = paths.world + ".tmp";
    Write(paths.header, header);
    Write(temp, world);
    HANDLE writer = CreateFileA(temp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(writer != INVALID_HANDLE_VALUE);
    const tmpl::File locked = store::Probe(temp);
    assert(locked.exists && locked.locked && locked.error == ERROR_SHARING_VIOLATION);
    assert(locked.size == world.size());
    tmpl::Publish publish;
    publish.Begin(tmpl::Origin::NewGame, 1000);
    publish.NativeReturned(true, 1, true, 1000);
    assert(publish.Poll(store::Observe(paths), 1000 + tmpl::RecoverAfterMs) == tmpl::Step::Wait);
    io::IoError error;
    events.clear();
    assert(!store::Recover(temp, paths.world, error, Trace));
    assert(!std::strcmp(error.operation, "recover-rename") && error.path == temp &&
           error.destination == paths.world && error.code == ERROR_SHARING_VIOLATION);
    assert(events.size() == 1 && !events[0].ok && events[0].code == ERROR_SHARING_VIOLATION);
    assert(fs::exists(temp) && !fs::exists(paths.world) && fs::file_size(temp) == world.size());
    io::Bytes held(world.size());
    DWORD read = 0;
    assert(ReadFile(writer, held.data(), static_cast<DWORD>(held.size()), &read, nullptr));
    assert(read == held.size() && held == world);
    assert(CloseHandle(writer));
    const tmpl::File unlocked = store::Probe(temp);
    assert(unlocked.exists && !unlocked.locked && unlocked.error == 0);
    const uint32_t candidate = 1000 + tmpl::RecoverAfterMs + tmpl::PollMs;
    assert(publish.Poll(store::Observe(paths), candidate) == tmpl::Step::Wait);
    assert(publish.Poll(store::Observe(paths), candidate + tmpl::RecoverStableMs - 1) == tmpl::Step::Wait);
    assert(publish.Poll(store::Observe(paths), candidate + tmpl::RecoverStableMs) == tmpl::Step::RecoverWorld);
    assert(store::Recover(temp, paths.world, error));
    assert(!fs::exists(temp) && Read(paths.world) == world);
    Clean(paths);
}

void Collision(store::Paths const& paths, io::Bytes const& header, io::Bytes const& world)
{
    Write(paths.header, header);
    Write(paths.marker, {1});
    Write(paths.world + ".tmp", world);
    assert(fs::create_directory(paths.world));
    const tmpl::Facts facts = store::Observe(paths);
    assert(facts.world.exists && facts.world.error == ERROR_DIRECTORY);
    io::IoError error;
    assert(!store::Recover(paths.world + ".tmp", paths.world, error));
    assert(!std::strcmp(error.operation, "recover-rename") && error.code == ERROR_ALREADY_EXISTS);
    assert(!store::RemoveOwn(paths, error));
    assert(!std::strcmp(error.operation, "delete") && error.path == paths.world && error.code != 0);
    assert(!fs::exists(paths.marker) && fs::is_directory(paths.world));
    assert(fs::remove(paths.world));
    Clean(paths);
}

void LockedCleanup(store::Paths const& paths, io::Bytes const& header, io::Bytes const& world)
{
    Halves(paths, header, world);
    Write(paths.marker, {1});
    HANDLE locked = CreateFileA(paths.world.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    io::IoError error;
    events.clear();
    assert(!store::RemoveOwn(paths, error, Trace));
    assert(!std::strcmp(error.operation, "delete") && error.path == paths.world);
    assert(error.code == ERROR_SHARING_VIOLATION || error.code == ERROR_ACCESS_DENIED);
    assert(events.size() == 3 && events[0].operation == "delete" && events[0].path == paths.marker &&
           events[0].ok && !events[0].code);
    assert(events[1].operation == "delete" && events[1].path == paths.header && events[1].ok && !events[1].code);
    assert(events[2].operation == "delete" && events[2].path == paths.world &&
           !events[2].ok && events[2].code == error.code && !fs::exists(paths.marker));
    assert(CloseHandle(locked));
    Clean(paths);
}
#endif

void WrongFolder(fs::path const& dir, tmpl::Identity const& id)
{
    const auto paths = Paths(dir / "nonexistent");
    const tmpl::File file = store::Probe(paths.header);
    assert(!file.exists);
#ifdef _WIN32
    assert(file.folderMissing && file.error == ERROR_PATH_NOT_FOUND);
    assert(tmpl::Publish::Reason(store::Observe(paths)) == tmpl::Fail::Folder);
#else
    assert(file.error == ENOENT);
#endif
    tmpl::Seal seal;
    io::IoError error;
    tmpl::Fail why = tmpl::Fail::None;
    assert(!store::SealFiles(paths, tmpl::Origin::NewGame, id, seal, error, why));
    assert(why == tmpl::Fail::NoHeader && !std::strcmp(error.operation, "read-open") && error.path == paths.header);
#ifdef _WIN32
    assert(error.code == ERROR_PATH_NOT_FOUND);
#else
    assert(error.code == ENOENT);
#endif
    Expect(store::Check(paths, id, true), tmpl::Health::Missing, tmpl::Why::NoFiles);
    store::Paths empty;
    assert(!store::RemoveOwn(empty, error) && !std::strcmp(error.operation, "remove-folder") && error.code == 3);
    CheckClean(paths);
}

void Faults(store::Paths const& paths, tmpl::Identity const& id, io::Bytes const& header, io::Bytes const& world)
{
    for (auto stage : {io::WriteStage::Opened, io::WriteStage::Written, io::WriteStage::Flushed, io::WriteStage::Closed}) {
        Halves(paths, header, world);
        observing = paths; failStage = stage; stages.clear();
        tmpl::Seal seal;
        io::IoError error;
        tmpl::Fail why = tmpl::Fail::None;
        assert(!store::SealFiles(paths, tmpl::Origin::NewGame, id, seal, error, why, Inject));
        const char* op = stage == io::WriteStage::Flushed ? "flush-injected" :
                         stage == io::WriteStage::Closed ? "close-injected" : "write-injected";
        assert(!std::strcmp(error.operation, op) && error.path == paths.marker + ".tmp");
        assert(!stages.empty() && stages.back() == stage);
#ifdef _WIN32
        assert(error.code == ERROR_DISK_FULL);
#else
        assert(error.code == ENOSPC);
#endif
        assert(why == tmpl::Fail::DiskFull && !fs::exists(paths.marker) && !fs::exists(paths.marker + ".tmp"));
        assert(Read(paths.header) == header && Read(paths.world) == world);
        Seal(paths, id, seal);
        Expect(store::Check(paths, id, true), tmpl::Health::Valid, tmpl::Why::None);
        Clean(paths);
    }
}

void FormatRefusal(store::Paths const& paths, tmpl::Identity const& id, io::Bytes const& header, io::Bytes const& world)
{
    for (bool badHeader : {true, false}) {
        io::Bytes headerBytes = header, worldBytes = world;
        if (badHeader) headerBytes[8] = 0;
        else template_fixture::Word(worldBytes, 12, 0);

        Halves(paths, headerBytes, worldBytes);
        tmpl::Seal seal;
        io::IoError error;
        tmpl::Fail why = tmpl::Fail::None;
        assert(!store::SealFiles(paths, tmpl::Origin::NewGame, id, seal, error, why));
        assert(why == (badHeader ? tmpl::Fail::HeaderEmpty : tmpl::Fail::WorldEmpty));
        assert(!std::strcmp(error.operation, badHeader ? "seal-header-format" : "seal-world-format"));
        assert(error.path == (badHeader ? paths.header : paths.world) && error.code == 0);
        assert(!fs::exists(paths.marker) && !fs::exists(paths.marker + ".tmp"));
        assert(Read(paths.header) == headerBytes && Read(paths.world) == worldBytes);
        Expect(store::Check(paths, id, true), tmpl::Health::Half, tmpl::Why::NoMarker);
        Clean(paths);

        Halves(paths, header, world);
        Seal(paths, id, seal);
        Write(badHeader ? paths.header : paths.world, badHeader ? headerBytes : worldBytes);
        const tmpl::Why expected = badHeader ? tmpl::Why::HeaderFormat : tmpl::Why::WorldFormat;
        Expect(store::Check(paths, id, false), tmpl::Health::Valid, tmpl::Why::None);
        Expect(store::Check(paths, id, true), tmpl::Health::Corrupt, expected); // format takes precedence over CRC
        if (badHeader) seal.headerCrc = io::Crc(headerBytes);
        else seal.worldCrc = io::Crc(worldBytes);
        WriteText(paths.marker, tmpl::Encode(seal));
        Expect(store::Check(paths, id, true), tmpl::Health::Corrupt, expected); // even with a matching CRC
        assert(Read(paths.header) == headerBytes && Read(paths.world) == worldBytes);
        Clean(paths);
    }
}

void Corruption(store::Paths const& paths, tmpl::Identity const& id, io::Bytes const& header, io::Bytes const& world)
{
    Halves(paths, header, world);
    tmpl::Seal seal;
    Seal(paths, id, seal);
    io::Bytes changed = world;
    changed[world.size() / 2] ^= 0x80;
    Write(paths.world, changed);
    Expect(store::Check(paths, id, false), tmpl::Health::Valid, tmpl::Why::None);
    Expect(store::Check(paths, id, true), tmpl::Health::Corrupt, tmpl::Why::CrcMismatch);
    fs::resize_file(paths.world, world.size() - 1);
    Expect(store::Check(paths, id, true), tmpl::Health::Corrupt, tmpl::Why::SizeMismatch);
    fs::resize_file(paths.world, tmpl::MinWorldBytes - 1);
    Expect(store::Check(paths, id, false), tmpl::Health::Corrupt, tmpl::Why::WorldSize);
    Write(paths.world, world);
    Write(paths.header, {});
    Expect(store::Check(paths, id, false), tmpl::Health::Corrupt, tmpl::Why::HeaderEmpty);
    Write(paths.header, header);
    tmpl::Seal stale = seal;
    ++stale.game;
    WriteText(paths.marker, tmpl::Encode(stale));
    Expect(store::Check(paths, id, false), tmpl::Health::Stale, tmpl::Why::GameChanged);
    assert(fs::remove(paths.marker));
    Expect(store::Check(paths, id, false), tmpl::Health::Half, tmpl::Why::NoMarker);
    WriteText(paths.marker, "garbage\n");
    Expect(store::Check(paths, id, false), tmpl::Health::Corrupt, tmpl::Why::MarkerFormat);
    Clean(paths);
}

void DeleteTrace(store::Paths const& paths)
{
    const std::vector<std::string> ordered{paths.marker, paths.header, paths.world,
                                           paths.marker + ".tmp", paths.header + ".tmp", paths.world + ".tmp"};
    for (auto const& path : ordered) Write(path, {1});
    events.clear();
    io::IoError error;
    assert(store::RemoveOwn(paths, error, Trace));
    assert(events.size() == ordered.size());
    for (size_t i = 0; i < ordered.size(); ++i)
        assert(events[i].operation == "delete" && events[i].path == ordered[i] && events[i].ok && !events[i].code);
    CheckClean(paths);
}

void ErrorMapping()
{
#ifdef _WIN32
    for (uint32_t code : {uint32_t(ERROR_DISK_FULL), uint32_t(ERROR_HANDLE_DISK_FULL)})
        assert(store::FailFor({"write", {}, {}, code}, tmpl::Fail::Seal) == tmpl::Fail::DiskFull);
    assert(store::FailFor({"write", {}, {}, ERROR_ACCESS_DENIED}, tmpl::Fail::Seal) == tmpl::Fail::AccessDenied);
    assert(store::FailFor({"write", {}, {}, ERROR_SHARING_VIOLATION}, tmpl::Fail::Seal) == tmpl::Fail::Busy);
    assert(store::FailFor({"write", {}, {}, ERROR_INVALID_DATA}, tmpl::Fail::NoWorld) == tmpl::Fail::NoWorld);
#else
    assert(store::FailFor({"write", {}, {}, ENOSPC}, tmpl::Fail::Seal) == tmpl::Fail::DiskFull);
    assert(store::FailFor({"write", {}, {}, EACCES}, tmpl::Fail::Seal) == tmpl::Fail::AccessDenied);
    assert(store::FailFor({"write", {}, {}, EBUSY}, tmpl::Fail::Seal) == tmpl::Fail::Busy);
    assert(store::FailFor({"write", {}, {}, EINVAL}, tmpl::Fail::NoWorld) == tmpl::Fail::NoWorld);
#endif
}
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    const fs::path dir = argv[1];
    assert(fs::is_directory(dir));
    const store::Paths paths = Paths(dir);
    const store::Paths alternative = Paths(dir, true);
    const tmpl::Identity id{0x100000123ull, 42};
    const auto header = template_fixture::Header(65 * 1024);
    const auto alternativeHeader = template_fixture::Header(65 * 1024, 0x00b0);
    const auto world = template_fixture::World(2 * 1024 * 1024);
    const io::Bytes decoy{0, 0xff, 12, 34, 56};
    const std::string quick = (dir / "QuickSave.g3savcp").string();
    const std::string foreignWorld = (dir / "G3_World_01_0001.g3savcp").string();
    Write(quick, decoy); Write(foreignWorld, decoy);
    Happy(paths, id, header, world); Decoys(quick, foreignWorld, decoy);
    Happy(alternative, id, alternativeHeader, world); Decoys(quick, foreignWorld, decoy);
#ifdef _WIN32
    RealAsync(paths, id, header, world); Decoys(quick, foreignWorld, decoy);
    SharedWriter(paths, header, world); Decoys(quick, foreignWorld, decoy);
#endif
    MissingHeader(paths, id, world); Decoys(quick, foreignWorld, decoy);
    Recovery(paths, world); Decoys(quick, foreignWorld, decoy);
#ifdef _WIN32
    Collision(paths, header, world); Decoys(quick, foreignWorld, decoy);
    LockedCleanup(paths, header, world); Decoys(quick, foreignWorld, decoy);
#endif
    WrongFolder(dir, id); Decoys(quick, foreignWorld, decoy);
    Faults(paths, id, header, world); Decoys(quick, foreignWorld, decoy);
    FormatRefusal(paths, id, header, world); Decoys(quick, foreignWorld, decoy);
    Corruption(paths, id, header, world); Decoys(quick, foreignWorld, decoy);
    DeleteTrace(paths); Decoys(quick, foreignWorld, decoy);
    ErrorMapping();
    CheckClean(paths); CheckClean(alternative);
    Decoys(quick, foreignWorld, decoy);
    return 0;
}
