// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/saving_store.h"
#include <cassert>
#include <filesystem>
#include <cstring>

using namespace g3t::saving;
using namespace g3t::saving::store;

static PublishStage failAt;
static uint32_t failureCode;
static bool Inject(PublishStage stage, IoError& error)
{
    return stage != failAt || Fail(&error, "injected-publish", "injected-path", failureCode);
}
static WriteStage writeAt;
static bool InjectWrite(WriteStage stage)
{
    if (stage != writeAt) return true;
#ifdef _WIN32
    SetLastError(failureCode);
#else
    errno = static_cast<int>(failureCode);
#endif
    return false;
}
static Bytes Hero(Key const& key)
{
    const std::string text = "G3THERO 4\nidentity " + key.world + " " + key.save + " " +
        std::to_string(key.player) + "\npose 1 2 3 0 0 0 1\ndowned 0\nend 0\n";
    return {text.begin(), text.end()};
}
static Bytes ReadBytes(std::string const& path)
{
    Bytes bytes;
    IoError error;
    assert(Read(path, bytes, &error));
    return bytes;
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string base = argv[1];
    const std::string paths[3] = {base + ".g3savcp", base + ".g3savcpdat", base + ".g3t"};
    const Key key{std::string(32, 'a'), std::string(32, 'b'), 11};
    const Save old{key, {{NativeHeader, 1, 0, {1,2,3}}, {NativeWorld, 1, 0, {4,5,6}},
                         {Journal, 1, 0, {}}, {Character, 4, 11, Hero(key)}}};
    auto next = old;
    next.blocks[0].bytes = {7,8,9};
    next.blocks[1].bytes = {10,11,12};
    Bytes before, after;
    assert(Encode(old, before) && Encode(next, after));
    IoError error, recovery;

    Bytes native;
    std::string source;
    assert(!ReadNative(paths[0], native, error, source));
    assert(Missing(error.code) && !std::strcmp(NativeReason(error), "native-files-missing"));
    assert(RetryNative(error, 0) && RetryNative(error, 2999) && !RetryNative(error, 3000));
    // A native success with an unchecked final rename can leave a complete .tmp.
    assert(Atomic(paths[0] + ".tmp", old.blocks[0].bytes));
    assert(ReadNative(paths[0], native, error, source) && source == paths[0] + ".tmp");
    assert(native == old.blocks[0].bytes && Remove(source));

#ifdef _WIN32
    const uint32_t denied = ERROR_ACCESS_DENIED, full = ERROR_DISK_FULL;
#else
    const uint32_t denied = EACCES, full = ENOSPC;
#endif
    for (auto stage : {PublishStage::Marker, PublishStage::World, PublishStage::Header, PublishStage::Container}) {
        for (auto code : {denied, full}) {
            assert(Publish(paths, old, before, error, recovery));
            failAt = stage; failureCode = code;
            assert(!Publish(paths, next, after, error, recovery, Inject));
            assert(error.code == code);
            assert(!std::strcmp(IoReason(error, "cache-publish-failed"),
                                code == denied ? "save-access-denied" : "disk-full"));
            assert(ReadBytes(paths[0]) == old.blocks[0].bytes);
            assert(ReadBytes(paths[1]) == old.blocks[1].bytes);
            assert(ReadBytes(paths[2]) == before);
            assert(!std::filesystem::exists(paths[2] + ".publish"));
            assert(!std::filesystem::exists(paths[0] + ".tmp"));
            assert(!std::filesystem::exists(paths[1] + ".tmp"));
            assert(!std::filesystem::exists(paths[2] + ".tmp"));
            assert(Publish(paths, next, after, error, recovery)); // No busy or poisoned slot after failure.
            assert(ReadBytes(paths[2]) == after);
        }
    }
    // Real atomic writer failures retain their originating Windows error after close/delete.
    for (auto stage : {WriteStage::Opened, WriteStage::Written, WriteStage::Flushed, WriteStage::Closed}) {
        for (auto code : {denied, full}) {
            assert(Atomic(paths[2], before));
            writeAt = stage; failureCode = code;
            assert(!Atomic(paths[2], after, InjectWrite, &error));
            assert(error.code == code && ReadBytes(paths[2]) == before);
            assert(!std::filesystem::exists(paths[2] + ".tmp"));
            assert(Atomic(paths[2], after));
        }
    }
#ifdef _WIN32
    // Actual Win32 writer lock: no native reader may capture an unfinished stream.
    HANDLE locked = CreateFileA(paths[1].c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!ReadNative(paths[1], native, error, source));
    assert(error.code == ERROR_SHARING_VIOLATION);
    assert(!std::strcmp(NativeReason(error), "native-files-busy") && RetryNative(error, 1));
    assert(CloseHandle(locked));
    assert(ReadNative(paths[1], native, error, source));
    assert(Publish(paths, next, after, error, recovery));
    for (auto code : {ERROR_HANDLE_DISK_FULL, ERROR_NOT_ENOUGH_QUOTA})
        assert(!std::strcmp(IoReason({"write", base, {}, static_cast<uint32_t>(code)},
                                   "container-write-failed"), "disk-full"));
#endif
    // Rename collision is an actual destination directory, not a simulated successful write.
    const std::string collision = base + ".collision";
    std::filesystem::create_directory(collision);
    assert(!Atomic(collision, after, nullptr, &error));
    assert(!std::strcmp(error.operation, "rename") && error.destination == collision && error.code);
    assert(std::filesystem::is_directory(collision) && !std::filesystem::exists(collision + ".tmp"));
    std::filesystem::remove(collision);
    assert(Atomic(collision, after) && Remove(collision));
    // An interrupted first publication must not leave a loadable native pair after recoverable failure.
    for (auto const& path : paths) assert(Remove(path));
    failAt = PublishStage::Header; failureCode = denied;
    assert(!Publish(paths, next, after, error, recovery, Inject));
    for (auto const& path : paths) assert(!std::filesystem::exists(path));
    assert(!std::filesystem::exists(paths[2] + ".publish"));
    assert(Publish(paths, next, after, error, recovery));
    Save decoded;
    assert(Decode(ReadBytes(paths[2]), decoded));
    assert(ReadBytes(paths[0]) == Find(decoded, NativeHeader)->bytes);
    assert(ReadBytes(paths[1]) == Find(decoded, NativeWorld)->bytes);
    // Loading repairs both caches from the authoritative unit; failed repair rolls back.
    const std::string caches[2] = {paths[0], paths[1]};
    failAt = PublishStage::Header; failureCode = denied;
    assert(!Repair(caches, old, error, recovery, Inject));
    assert(ReadBytes(paths[0]) == next.blocks[0].bytes && ReadBytes(paths[1]) == next.blocks[1].bytes);
    assert(Atomic(paths[2] + ".publish", {'G','3','T','P','U','B','1'}));
    assert(Repair(caches, decoded, error, recovery));
    assert(Remove(paths[2] + ".publish"));
    assert(ReadBytes(paths[2]) == after);
    assert(Remove(paths[0]) && Remove(paths[1]));
    assert(Repair(caches, decoded, error, recovery));
    assert(ReadBytes(paths[0]) == Find(decoded, NativeHeader)->bytes);
    assert(ReadBytes(paths[1]) == Find(decoded, NativeWorld)->bytes);
}
