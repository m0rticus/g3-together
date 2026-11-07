// SPDX-License-Identifier: GPL-3.0-or-later
#include "persist.h"

#include "ai.h"
#include "config.h"
#include "containers.h"
#include "dialogaudio.h"
#include "downed.h"
#include "hero.h"
#include "hud.h"
#include "i18n.h"
#include "journal.h"
#include "log.h"
#include "lobby.h"
#include "net.h"
#include "npcs.h"
#include "remote.h"
#include "saving_native.h"
#include "saving_policy.h"
#include "steam.h"
#include "trade.h"
#include "world.h"

#include <Game.h>
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace g3t::persist {
namespace {
constexpr size_t Stolen = saving::native::Stolen;
constexpr uint32_t FileLimit = 64u * 1024 * 1024;
constexpr uint32_t PrepareLimit = 15000, WriteLimit = 60000, TransferLimit = 180000;
constexpr uint32_t PreparedLimit = PrepareLimit + WriteLimit + TransferLimit;
constexpr UINT_PTR SaveTimer = 0x47335453, AutoloadTimer = 0x47335436;
using FileFn = bool(__stdcall*)(void*, bCString const*);
using Getter = void*(__cdecl*)();
using WorldSaveFn = bool(__thiscall*)(gCWorld*, bCString const&, eCGfxImageData*, void*, GEU32);
using WorldLoadFn = bool(__thiscall*)(gCWorld*, bCString const&, bool);
FileFn originalSave = nullptr, originalLoad = nullptr;
Getter fileManagerGetter = nullptr;
void* fileManagerSingleton = nullptr;
void* fileManagerVtable = nullptr;
WorldSaveFn originalWorldSave = nullptr;
WorldLoadFn originalWorldLoad = nullptr;
unsigned lowerLoadCalls = 0;
bool lowerLoadGood = false;
unsigned char* saveTarget = nullptr;
unsigned char* loadTarget = nullptr;
bool installed = false, pairedEnabled = false, writing = true, transferring = true, syncing = true;
bool earlyGate = true, byGeneration = true, resendGuard = true, loadTicks = false, autoloadHold = false;
uint32_t pausedUntil = 0;
unsigned processDepth = 0, nativeLoadDepth = 0, nativeSaveDepth = 0;
unsigned saves = 0, loads = 0, sidecarsWritten = 0, sidecarsRead = 0;
unsigned recordsWritten = 0, recordsApplied = 0, filesSent = 0, filesReceived = 0, autoLoads = 0;
unsigned loadFrames = 0, loadFramesRan = 0, autoloadSerial = 0, autoloadPosted = 0, autoloadStale = 0;
unsigned epochs = 0, worldChanges = 0, readies = 0, others = 0;
std::string lastSave, lastLoad, lastReceived = "-", campaign, loading;
uint8_t localKind = net::KindMenu;
std::string localSave;
bool newWorld = false, ownCharacterReady = false, everIn = false;
uint32_t epoch = 0, settledAt = 0, synced = 0, loadedEpoch = 0, hinted = 0;
net::WorldNote hostWorld{};

struct Manifest {
    saving::Key key; // player is the owner of the native hero, not the recipient's character
    uint32_t size[2] = {}, crc[2] = {};
    std::string legacy;
};
Manifest current;
struct Client {
    uint32_t generation = 0, told = 0, ready = 0, sent = 0, sentAt = 0, hinted = 0;
    std::string toldCampaign;
    bool wants = false, arrived = false;
    unsigned resends = 0;
};
Client clients[net::MaxPlayers]{};

saving::Barrier barrier;
Manifest savingManifest;
std::string requestedSlot;
std::vector<uint8_t> savedJournal;
uint32_t saveStarted = 0, nativeReturnedAt = 0;
HWND saveWindow = nullptr;
bool aliasPublished = false;

struct Prepared {
    net::SaveNote note{};
    uint32_t welcome = 0, since = 0;
    bool valid = false;
};
Prepared prepared;
net::SaveNote incomingNote{};
uint32_t incomingWelcome = 0;
bool receiving = false, incomingFailed = false;
std::vector<uint8_t> incoming[net::FileCount];
uint32_t incomingTotal[net::FileCount]{}, incomingAt = 0;
uint32_t requestAt = 0, requestWelcome = 0;
bool requestPending = false;
std::string requestId;

std::string pendingRecord, refusedRecord, waitingImport;
saving::Key pendingKey, desiredKey;
bool pendingLegacy = false;
uint32_t pendingSince = 0, refusedHinted = 0;
uint32_t nextImportTry = 0, waitingEpoch = 0, waitingWelcome = 0, nextRestoreTry = 0;
FILETIME refusedWritten{};
struct Autoload {
    std::string slot;
    uint32_t epoch = 0, welcome = 0;
    unsigned loads = 0, serial = 0;
    HWND window = nullptr;
    bool valid = false, armed = false;
};
Autoload autoload;

void Evaluate();
void OrderAutoload(std::string const& slot);
void CancelSave(char const* why);
bool __stdcall LoadDetour(void* self, bCString const* name);

bool Readable(void const* address, size_t size)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (!start || size > UINTPTR_MAX - start) return false;
    for (uintptr_t at = start; at < start + size;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void const*>(at), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            info.Protect == PAGE_EXECUTE) return false;
        const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (end <= at) return false;
        at = end;
    }
    return true;
}

bool ResolveNative(unsigned char* (&resolved)[5])
{
    HMODULE game = GetModuleHandleA("Game.dll");
    auto* image = reinterpret_cast<unsigned char*>(game);
    if (!Readable(image, sizeof(IMAGE_DOS_HEADER))) { Log("persist: unresolved Game.dll; paired saves off"); return false; }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        !Readable(image + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS32))) {
        Log("persist: unresolved Game.dll PE header; paired saves off");
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || !size || size > 128u * 1024 * 1024 ||
        !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96 ||
        !Readable(sections, nt->FileHeader.NumberOfSections * sizeof(*sections))) {
        Log("persist: unresolved Game.dll PE layout; paired saves off");
        return false;
    }
    char path[MAX_PATH]{};
    GetModuleFileNameA(game, path, MAX_PATH);
    Logf("persist: native module=%s base=%p image_size=%lu timestamp=%08lx",
         path, image, static_cast<unsigned long>(size), nt->FileHeader.TimeDateStamp);
    size_t sites[5]{};
    bool good = true;
    for (size_t i = 0; i < 5; ++i) {
        auto const& fn = saving::native::Sites[i];
        unsigned matches = 0;
        sites[i] = saving::native::Missing;
        for (unsigned section = 0; section < nt->FileHeader.NumberOfSections; ++section) {
            auto const& part = sections[section];
            if (!(part.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const size_t length = part.Misc.VirtualSize;
            if (!saving::native::Range(size, part.VirtualAddress, length) ||
                !Readable(image + part.VirtualAddress, length)) continue;
            unsigned count = 0;
            const size_t at = saving::native::FindUnique(image + part.VirtualAddress, length, fn.bytes, count);
            if (at != saving::native::Missing) sites[i] = part.VirtualAddress + at;
            matches += count;
        }
        if (matches == 1 && sites[i] != saving::native::Missing &&
            (!fn.symbol || reinterpret_cast<unsigned char*>(GetProcAddress(game, fn.symbol)) == image + sites[i])) {
            resolved[i] = image + sites[i];
            continue;
        }
        char bytes[25] = "unreadable";
        if (saving::native::Range(size, fn.hint, 12) && Readable(image + fn.hint, 12))
            for (size_t b = 0; b < 12; ++b) Format(bytes + b * 2, 3, "%02x", image[fn.hint + b]);
        Logf("persist: unresolved %s matches=%u hint=%08lx live=%s match=%p export=%p; paired saves off",
             fn.name, matches, static_cast<unsigned long>(fn.hint), bytes,
             sites[i] == saving::native::Missing ? nullptr : image + sites[i],
             fn.symbol ? GetProcAddress(game, fn.symbol) : nullptr);
        good = false;
    }
    if (!good) return false;
    const bool saveLink = Readable(resolved[0] + 0xb9, 5) && saving::native::Calls(image, size, sites[0] + 0xb9, sites[2]);
    const bool loadLink = Readable(resolved[1] + 0x2b2, 5) && saving::native::Calls(image, size, sites[1] + 0x2b2, sites[3]);
    if (!saveLink) Log("persist: unresolved SaveGameFn->WorldSaveGameFn call edge; paired saves off");
    if (!loadLink) Log("persist: unresolved LoadGameFn->WorldLoadGameFn call edge; paired saves off");
    if (!saveLink || !loadLink) return false;
    size_t singleton = 0, vtable = 0;
    const uintptr_t table = saving::native::Word(image, sites[4] + 32);
    if (!Readable(reinterpret_cast<void const*>(table), sizeof(void*)) ||
        !saving::native::Links(image, size, reinterpret_cast<uintptr_t>(image), sites, singleton, vtable) ||
        !Readable(image + singleton, 12) || !Readable(image + vtable, sizeof(void*))) {
        Log("persist: unresolved FileManagerGetter singleton/vtable layout; paired saves off");
        return false;
    }
    fileManagerGetter = reinterpret_cast<Getter>(resolved[4]);
    fileManagerSingleton = image + singleton;
    fileManagerVtable = image + vtable;
    return true;
}

void* FileManager(bool forLoad)
{
    if (!pairedEnabled || !fileManagerGetter || !Readable(fileManagerSingleton, 12)) return nullptr;
    void* self = fileManagerGetter();
    void* vtable = nullptr;
    void* menu = nullptr;
    if (self != fileManagerSingleton) return nullptr;
    std::memcpy(&vtable, self, sizeof(vtable));
    std::memcpy(&menu, static_cast<unsigned char*>(self) + 4, sizeof(menu));
    if (vtable != fileManagerVtable || (forLoad && !Readable(menu, 0x4c))) {
        Logf("persist: native FileManagerGetter refused singleton/vtable/main-menu load=%d player=%llu",
             forLoad ? 1 : 0, steam::PlayerId());
        return nullptr;
    }
    return self;
}

bool SafeName(char const* name) { return saving::SafeName(name); }
std::string Folder()
{
    std::string folder = gCWorld::GetSaveGameFolder().GetText();
    if (!folder.empty() && folder.back() != '\\' && folder.back() != '/') folder += '\\';
    return folder;
}
char const* SaveExtension(int file)
{
    static char const* const normal[] = {".g3savcp", ".g3savcpdat", ".g3t"};
    static char const* const alternative[] = {".g3savcpx", ".g3savcpxdat", ".g3t"};
    return eCApplication::GetInstance().GetEngineSetup().AlternativeBalancing ? alternative[file] : normal[file];
}
std::string Path(std::string const& slot, int file)
{
    const std::string folder = Folder();
    return folder.empty() ? std::string() : folder + slot + SaveExtension(file);
}
bool Exists(std::string const& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void Notice(i18n::Message message, bool bad = false)
{
    if (gCSession::GetInstance().IsGameRunning()) hud::Message(bad ? hud::Warning : hud::Info, i18n::Text(message));
    else hud::MenuLine(i18n::Text(message));
}

uint32_t Crc32(uint8_t const* data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return crc ^ 0xffffffffu;
}

bool ReadAll(std::string const& path, std::vector<uint8_t>& out)
{
    out.clear();
    // Sharing only with readers also checks that no native writer still holds this file.
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= FileLimit;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
    }
    CloseHandle(file);
    if (!ok) out.clear();
    return ok;
}

bool WriteAtomic(std::string const& path, std::vector<uint8_t> const& data)
{
    if (path.empty()) { Log("persist: checkpoint path unavailable; paired saves off for this save"); return false; }
    const std::string temp = path + ".tmp";
    FILE* file = nullptr;
    bool ok = !fopen_s(&file, temp.c_str(), "wb") && file;
    if (ok) {
        ok = fwrite(data.data(), 1, data.size(), file) == data.size();
        ok = fflush(file) == 0 && ok;
        ok = fclose(file) == 0 && ok;
    }
    if (ok) ok = MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        DeleteFileA(temp.c_str());
        Logf("persist: atomic checkpoint write failed: %s (error %lu)", path.c_str(), GetLastError());
    }
    return ok;
}

std::string HexId(uint8_t const* bytes)
{
    char text[33]{};
    for (int i = 0; i < 16; ++i) Format(text + 2 * i, 3, "%02x", bytes[i]);
    return text;
}
std::string RandomId()
{
    uint8_t bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        Log("persist: checkpoint identity generation failed");
        return {};
    }
    return HexId(bytes);
}
std::string LegacyId(std::vector<uint8_t> const (&data)[net::FileCount])
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD length = 0, returned = 0;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &returned, 0) >= 0;
    std::vector<uint8_t> object(length);
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) >= 0;
    // Length-prefix the two files; their boundary is part of the legacy identity.
    for (int f = 0; ok && f < 2; ++f) {
        const uint32_t size = static_cast<uint32_t>(data[f].size());
        ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<uint32_t*>(&size)), sizeof(size), 0) >= 0 &&
             BCryptHashData(hash, const_cast<PUCHAR>(data[f].data()), size, 0) >= 0;
    }
    uint8_t digest[32]{};
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) { Log("persist: legacy checkpoint fingerprint failed"); return {}; }
    return HexId(digest);
}

bool ParseManifest(std::vector<uint8_t> const& bytes, Manifest& manifest)
{
    if (bytes.empty()) return false;
    std::istringstream input(std::string(bytes.begin(), bytes.end()));
    std::string line, tag, extra;
    if (!std::getline(input, line) || (line != "G3T 2" && line != "G3T 2\r") || !std::getline(input, line)) return false;
    Manifest found;
    std::istringstream fields(line);
    if (!(fields >> tag >> found.key.world >> found.key.save >> found.key.player >>
          found.size[0] >> found.crc[0] >> found.size[1] >> found.crc[1]) || fields >> extra ||
        tag != "checkpoint" || !saving::Valid(found.key) || !found.size[0] || !found.size[1] ||
        found.size[0] > FileLimit || found.size[1] > FileLimit) return false;
    bool ended = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (ended && !line.empty()) return false;
        if (line.compare(0, 7, "legacy ") == 0) {
            found.legacy = line.substr(7);
            if (!SafeName(found.legacy.c_str())) return false;
        }
        if (line.compare(0, 15, "checkpoint_end ") == 0) {
            if (line.substr(15) != found.key.save || ended) return false;
            ended = true;
        }
    }
    if (!ended) return false;
    manifest = found;
    return true;
}

bool ManifestAt(std::string const& slot, Manifest& manifest)
{
    std::vector<uint8_t> bytes;
    return ReadAll(Path(slot, 2), bytes) && ParseManifest(bytes, manifest);
}
bool Verify(Manifest const& manifest, std::vector<uint8_t> const (&data)[net::FileCount])
{
    for (int f = 0; f < 2; ++f)
        if (manifest.size[f] != data[f].size() || manifest.crc[f] != Crc32(data[f].data(), data[f].size())) return false;
    return true;
}
bool ReadBundle(std::string const& slot, Manifest& manifest, std::vector<uint8_t> (&data)[net::FileCount])
{
    for (int f = 0; f < net::FileCount; ++f) if (!ReadAll(Path(slot, f), data[f])) return false;
    return ParseManifest(data[2], manifest) && Verify(manifest, data);
}
std::vector<uint8_t> ManifestBytes(Manifest const& manifest, std::vector<uint8_t> const& journal)
{
    std::ostringstream text;
    text << "G3T 2\ncheckpoint " << manifest.key.world << ' ' << manifest.key.save << ' ' << manifest.key.player << ' '
         << manifest.size[0] << ' ' << manifest.crc[0] << ' ' << manifest.size[1] << ' ' << manifest.crc[1] << '\n';
    if (!manifest.legacy.empty()) text << "legacy " << manifest.legacy << '\n';
    text << std::string(journal.begin(), journal.end());
    text << "\ncheckpoint_end " << manifest.key.save << '\n';
    const std::string bytes = text.str();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}
bool StoreBundle(std::string const& slot, std::vector<uint8_t> const (&data)[net::FileCount])
{
    // Immutable checkpoint names plus a manifest-last publication make interrupted copies detectable.
    for (int f = 0; f < net::FileCount; ++f) if (!WriteAtomic(Path(slot, f), data[f])) return false;
    return true;
}

std::string CharacterPath(saving::Key const& key)
{
    const std::string folder = Folder();
    return folder.empty() ? std::string() : folder + saving::RecordName(key, false);
}
std::string LatestPath(saving::Key const& key)
{
    const std::string folder = Folder();
    return folder.empty() ? std::string() : folder + saving::RecordName(key, true);
}
bool PublishLatest(saving::Key const& key)
{
    std::vector<uint8_t> bytes;
    if (!ReadAll(CharacterPath(key), bytes) || !WriteAtomic(LatestPath(key), bytes)) {
        Logf("persist: latest character not published world=%s save=%s player=%llu", key.world.c_str(), key.save.c_str(), key.player);
        return false;
    }
    return true;
}
bool CharacterDigest(char const* phase, saving::Key const& key, std::string const& path, bool applied = true)
{
    std::vector<uint8_t> data;
    const bool ok = applied && ReadAll(path, data) && !data.empty();
    Logf("persist: character %s world=%s save=%s player=%llu ok=%d size=%zu crc=%08x",
         phase, key.world.c_str(), key.save.c_str(), key.player, ok ? 1 : 0, data.size(),
         ok ? Crc32(data.data(), data.size()) : 0);
    return ok;
}
bool CaptureCharacter(saving::Key const& key)
{
    const std::string path = CharacterPath(key);
    const bool ok = hero::SaveRecord(path.c_str(), key);
    return CharacterDigest("capture", key, path, ok);
}
bool StableRunningHero()
{
    gCSession& game = gCSession::GetInstance();
    return ownCharacterReady && trade::ReadyForSave() && !nativeLoadDepth && !nativeSaveDepth && loading.empty() &&
           pendingRecord.empty() && refusedRecord.empty() && game.GetHero() && game.IsGameRunning();
}
bool NativeSaveAllowed()
{
    if (!eCApplication::IsInitialised() || nativeLoadDepth || nativeSaveDepth || !loading.empty()) return false;
    gCSession& session = gCSession::GetInstance();
    if (!session.IsGameRunning() || !session.GetHero() || session.GetPlayer() != session.GetHero() ||
        downed::Held() || downed::NativeDown()) return false;
    gCWorld* world = gCWorld::GetCurrentWorld();
    if (!world || !world->GetWorldEntity() || !gCInfoManager_PS::GetInstance() || Folder().empty()) return false;
    return session.IsSaveAllowed();
}

net::SaveNote Note(uint8_t op, Manifest const& manifest, bool ok = true)
{
    net::SaveNote note{};
    note.op = op;
    note.ok = ok ? 1 : 0;
    note.epoch = net::IsHost() ? epoch : hostWorld.epoch;
    strncpy_s(note.world, manifest.key.world.c_str(), _TRUNCATE);
    strncpy_s(note.save, manifest.key.save.c_str(), _TRUNCATE);
    const std::string slot = saving::Slot(manifest.key.save);
    strncpy_s(note.name, slot.c_str(), _TRUNCATE);
    return note;
}
void ClearIncoming()
{
    receiving = false;
    incomingFailed = false;
    for (auto& bytes : incoming) bytes.clear();
    std::fill(std::begin(incomingTotal), std::end(incomingTotal), 0);
    incomingAt = 0;
}

bool SendCampaign(uint8_t to, bool commit = false)
{
    if (net::FileTransferBusy(to)) return false;
    Manifest manifest;
    std::vector<uint8_t> data[net::FileCount];
    if (!pairedEnabled || !transferring || !ReadBundle(campaign, manifest, data)) {
        Logf("persist: checkpoint %s is incomplete; not sent to player %u", campaign.c_str(), to);
        return false;
    }
    const net::SaveNote note = Note(commit ? net::SaveCommit : net::SaveImport, manifest);
    uint32_t sizes[net::FileCount]{}, crcs[net::FileCount]{};
    for (int f = 0; f < net::FileCount; ++f) {
        sizes[f] = static_cast<uint32_t>(data[f].size());
        crcs[f] = Crc32(data[f].data(), data[f].size());
    }
    if (!net::QueueFileTransfer(to, note, campaign.c_str(), data, sizes, crcs)) return false;
    ++filesSent;
    clients[to].sentAt = GetTickCount();
    clients[to].arrived = false;
    Logf("persist: checkpoint sent world=%s save=%s native_owner=%llu player_slot=%u mode=%s size=%u/%u/%u crc=%08x/%08x/%08x",
         manifest.key.world.c_str(), manifest.key.save.c_str(), manifest.key.player, to, commit ? "store" : "load",
         sizes[0], sizes[1], sizes[2], crcs[0], crcs[1], crcs[2]);
    return true;
}

bool AdoptLegacy(std::string const& slot, Manifest& manifest)
{
    std::vector<uint8_t> data[net::FileCount];
    if (!ReadAll(Path(slot, 0), data[0]) || !ReadAll(Path(slot, 1), data[1]) || data[0].empty() || data[1].empty()) return false;
    // A malformed modern manifest must never be treated as an old save.
    if (Exists(Path(slot, 2))) {
        if (!ReadAll(Path(slot, 2), data[2])) return false;
        const std::string text(data[2].begin(), data[2].end());
        if (text.compare(0, 5, "G3T 2") == 0) return false;
        if (!data[2].empty() && text.compare(0, 5, "G3T 1") != 0) return false;
    }
    manifest.key.world = LegacyId(data);
    manifest.key.save = manifest.key.world;
    // An old transferred blob has somebody else's native hero, even when its host ID was never recorded.
    manifest.key.player = saving::LegacyNativeOwner(slot, steam::PlayerId());
    manifest.legacy = slot;
    if (!saving::Valid(manifest.key)) return false;
    for (int f = 0; f < 2; ++f) {
        manifest.size[f] = static_cast<uint32_t>(data[f].size());
        manifest.crc[f] = Crc32(data[f].data(), data[f].size());
    }
    const std::vector<uint8_t> oldJournal = data[2];
    data[2] = ManifestBytes(manifest, oldJournal);
    const std::string canonical = saving::Slot(manifest.key.save);
    if (!StoreBundle(canonical, data)) return false;
    Logf("persist: legacy world adopted without changing source slot=%s world=%s save=%s", slot.c_str(),
         manifest.key.world.c_str(), manifest.key.save.c_str());
    return true;
}

bool ClaimLegacy(std::string const& path)
{
    const std::string ownerPath = path + ".owner";
    std::vector<uint8_t> bytes;
    const std::string owner = std::to_string(steam::PlayerId()) + "\n";
    if (Exists(ownerPath)) return ReadAll(ownerPath, bytes) && std::string(bytes.begin(), bytes.end()) == owner;
    return steam::PlayerId() && WriteAtomic(ownerPath, std::vector<uint8_t>(owner.begin(), owner.end()));
}

bool ChooseCharacter(std::string const& slot, Manifest const& manifest)
{
    desiredKey = {manifest.key.world, manifest.key.save, steam::PlayerId()};
    if (!saving::Valid(desiredKey)) return false;
    const std::string exact = CharacterPath(desiredKey), latest = LatestPath(desiredKey);
    std::string legacy;
    if (!manifest.legacy.empty()) {
        const std::string oldName = saving::Prefix(manifest.legacy, "G3T_host_") ? manifest.legacy : "G3T_host_" + manifest.legacy;
        legacy = hero::RecordPath(oldName.c_str());
    } else if (saving::Prefix(slot, "G3T_host_")) legacy = hero::RecordPath(slot.c_str());
    const auto selection = saving::Select(Exists(exact), Exists(latest), !legacy.empty() && Exists(legacy),
                                         StableRunningHero() && NativeSaveAllowed());
    pendingLegacy = false;
    pendingKey = desiredKey;
    if (selection == saving::Restore::Exact || selection == saving::Restore::Latest) {
        pendingRecord = selection == saving::Restore::Exact ? exact : latest;
        saving::Key found;
        if (!hero::ReadRecordKey(pendingRecord.c_str(), found) || !saving::SameCharacter(found, desiredKey) ||
            (selection == saving::Restore::Exact && !saving::Exact(found, desiredKey))) {
            Logf("persist: character identity refused world=%s save=%s player=%llu record=%s", desiredKey.world.c_str(),
                 desiredKey.save.c_str(), desiredKey.player, pendingRecord.c_str());
            pendingRecord.clear();
            Notice(i18n::SaveCharacterMissing, true);
            return false;
        }
        pendingKey = found;
        if (selection == saving::Restore::Latest) {
            Logf("persist: missing exact character, recovering same-account/campaign record world=%s requested=%s recovered=%s player=%llu",
                 desiredKey.world.c_str(), desiredKey.save.c_str(), found.save.c_str(), found.player);
            Notice(i18n::SaveCharacterRecovered, true);
        }
    } else if (selection == saving::Restore::Legacy) {
        if (!ClaimLegacy(legacy)) {
            Logf("persist: legacy character belongs to another account or cannot be claimed: %s", legacy.c_str());
            Notice(i18n::SaveCharacterMissing, true);
            return false;
        }
        pendingRecord = legacy;
        pendingLegacy = true;
        Logf("persist: exact legacy character migration world=%s save=%s player=%llu source=%s", desiredKey.world.c_str(),
             desiredKey.save.c_str(), desiredKey.player, legacy.c_str());
    } else if (selection == saving::Restore::NewHero) {
        if (!CaptureCharacter(desiredKey)) {
            Log("persist: clean local character could not be captured before world import");
            Notice(i18n::SaveFailed, true);
            return false;
        }
        ++recordsWritten;
        pendingRecord = exact;
        Logf("persist: captured clean local character before import world=%s save=%s player=%llu",
             desiredKey.world.c_str(), desiredKey.save.c_str(), desiredKey.player);
    } else {
        Logf("persist: no own character world=%s save=%s player=%llu; waiting for a new/local character, never using native host hero",
             desiredKey.world.c_str(), desiredKey.save.c_str(), desiredKey.player);
        Notice(i18n::SaveCharacterMissing, true);
        return false;
    }
    if (!(pendingLegacy ? hero::ValidateLegacyRecord(pendingRecord.c_str()) :
                          hero::ValidateRecord(pendingRecord.c_str(), pendingKey))) {
        Logf("persist: character preflight refused world=%s save=%s player=%llu reason=%s; native load not invoked",
             desiredKey.world.c_str(), desiredKey.save.c_str(), desiredKey.player,
             hero::LastRefusal() ? hero::LastRefusal() : "record validation failed");
        pendingRecord.clear();
        Notice(i18n::SaveCharacterMissing, true);
        return false;
    }
    pendingSince = 0;
    return true;
}

void OnFileChunk(uint8_t file, uint32_t total, uint32_t offset, uint8_t const* data, uint16_t length)
{
    if (!receiving || incomingWelcome != net::WelcomeSerial() || incomingNote.epoch != hostWorld.epoch) return;
    if (incomingFailed) return;
    if (file >= net::FileCount || !data ||
        !saving::ChunkFits(incomingTotal[file], incoming[file].size(), total, offset, length, FileLimit)) {
        incomingFailed = true;
        Logf("persist: checkpoint chunk rejected save=%s player=%llu file=%u total=%u offset=%u size=%u",
             incomingNote.save, steam::PlayerId(), file, total, offset, length);
        return;
    }
    incomingTotal[file] = total;
    incoming[file].insert(incoming[file].end(), data, data + length);
    if (prepared.valid) prepared.since = GetTickCount();
}

void OnFileDone(char const* name, uint32_t const size[net::FileCount], uint32_t const crc[net::FileCount])
{
    if (!receiving || incomingWelcome != net::WelcomeSerial() || incomingNote.epoch != hostWorld.epoch) { ClearIncoming(); return; }
    bool ok = !incomingFailed && SafeName(name) && name == saving::Slot(incomingNote.save);
    for (int f = 0; f < net::FileCount; ++f)
        if (!size[f] || incomingTotal[f] != size[f] || incoming[f].size() != size[f] ||
            Crc32(incoming[f].data(), incoming[f].size()) != crc[f]) ok = false;
    Manifest manifest;
    ok = ok && ParseManifest(incoming[2], manifest) && Verify(manifest, incoming) &&
         manifest.key.world == incomingNote.world && manifest.key.save == incomingNote.save;
    const bool commit = incomingNote.op == net::SaveCommit;
    if (commit) {
        saving::Key key{incomingNote.world, incomingNote.save, steam::PlayerId()}, found;
        ok = ok && prepared.valid && prepared.welcome == net::WelcomeSerial() &&
             !std::strcmp(prepared.note.save, incomingNote.save) && hero::ReadRecordKey(CharacterPath(key).c_str(), found) &&
             saving::Exact(key, found);
    }
    if (ok) ok = StoreBundle(name, incoming);
    if (ok && commit) {
        const char* alias = prepared.note.name;
        ok = SafeName(alias) && !saving::Prefix(alias, "G3T_") && StoreBundle(alias, incoming);
        if (!ok) Logf("persist: client checkpoint alias not published save=%s slot=%s player=%llu",
                      incomingNote.save, alias, steam::PlayerId());
    }
    if (!ok) {
        Logf("persist: checkpoint transfer refused save=%s player=%llu mode=%s size=%u/%u/%u crc=%08x/%08x/%08x",
             incomingNote.save, steam::PlayerId(), commit ? "store" : "load",
             size[0], size[1], size[2], crc[0], crc[1], crc[2]);
        Notice(i18n::SaveFailed, true);
        if (commit) {
            net::SaveNote answer = incomingNote;
            answer.op = net::SaveStored;
            answer.ok = 0;
            net::SendSave(1, answer);
        }
        ClearIncoming();
        return;
    }
    ++filesReceived;
    lastReceived = name;
    Logf("persist: checkpoint stored world=%s save=%s player=%llu size=%u/%u/%u crc=%08x/%08x/%08x mode=%s",
         manifest.key.world.c_str(), manifest.key.save.c_str(), steam::PlayerId(),
         size[0], size[1], size[2], crc[0], crc[1], crc[2], commit ? "store" : "load");
    ClearIncoming();
    if (commit) {
        const saving::Key key{manifest.key.world, manifest.key.save, steam::PlayerId()};
        const bool latestOk = PublishLatest(key);
        if (latestOk) {
            current = manifest;
            localKind = net::KindSaved;
            localSave = name;
        }
        net::SendSave(1, Note(net::SaveStored, manifest, latestOk));
    } else {
        waitingImport = name;
        waitingEpoch = hostWorld.epoch;
        waitingWelcome = net::WelcomeSerial();
        nextImportTry = GetTickCount() + 2000;
        if (ChooseCharacter(name, manifest)) {
            // Selection is made again at the actual load. Never replace an exact record with the running hero.
            pendingRecord.clear();
            OrderAutoload(name);
        }
    }
}

void StopSaveTimer()
{
    if (saveWindow) KillTimer(saveWindow, SaveTimer);
    saveWindow = nullptr;
}
void FinishSave(bool ok, char const* why)
{
    if (!ok) net::CancelFileTransfers();
    Logf("persist: checkpoint %s world=%s save=%s player=%llu slot=%s reason=%s",
         ok ? "paired" : "incomplete", savingManifest.key.world.c_str(), savingManifest.key.save.c_str(),
         steam::PlayerId(), requestedSlot.c_str(), why);
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        if (!barrier.members[i].generation || !net::PeerJoined(i) || net::PeerGeneration(i) != barrier.members[i].generation) continue;
        net::SendSave(i, Note(ok ? net::SaveResult : net::SaveCancel, savingManifest, ok));
    }
    Notice(ok ? i18n::SavePaired : i18n::SaveFailed, !ok);
    StopSaveTimer();
    barrier = saving::Barrier{};
    savedJournal.clear();
}
void CancelSave(char const* why) { if (barrier.phase != saving::Barrier::Idle) FinishSave(false, why); }

bool CaptureJournal(std::string const& slot)
{
    const std::string path = Path(slot, 2) + ".state";
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") || !file) { Logf("persist: cannot capture world journal %s", path.c_str()); return false; }
    journal::Write(file);
    npcs::WriteCompanions(file);
    if (net::IsHost()) fprintf(file, "downed %d\n", downed::Held() ? 1 : 0);
    bool ok = !ferror(file);
    ok = fflush(file) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (ok) ok = ReadAll(path, savedJournal);
    DeleteFileA(path.c_str());
    return ok;
}

void CALLBACK SaveNow(HWND, UINT, UINT_PTR, DWORD)
{
    if (processDepth || nativeLoadDepth || nativeSaveDepth || barrier.phase != saving::Barrier::Preparing || !barrier.AllReady()) return;
    if (barrier.epoch != epoch || !StableRunningHero() || !NativeSaveAllowed()) { CancelSave("host no longer ready"); return; }
    const std::string slot = saving::Slot(barrier.save);
    void* manager = FileManager(false);
    if (!manager || !originalSave || !writing || !CaptureJournal(slot) || !CaptureCharacter(savingManifest.key)) {
        CancelSave("host character/journal/native save API unavailable");
        return;
    }
    ++recordsWritten;
    remote::DespawnAll("coordinated save");
    const bCString name(slot.c_str());
    Logf("persist: native invoke=HUD.SaveGame world=%s save=%s player=%llu slot=%s",
         savingManifest.key.world.c_str(), savingManifest.key.save.c_str(), savingManifest.key.player, slot.c_str());
    ++nativeSaveDepth;
    const bool ok = originalSave(manager, &name);
    --nativeSaveDepth;
    ++saves;
    Logf("persist: native result=HUD.SaveGame save=%s player=%llu ok=%d", savingManifest.key.save.c_str(),
         savingManifest.key.player, ok ? 1 : 0);
    if (!ok) { CancelSave("native host save failed"); return; }
    nativeReturnedAt = GetTickCount();
    barrier.phase = saving::Barrier::Writing;
    Logf("persist: native checkpoint save returned world=%s save=%s player=%llu; waiting for disk completion",
         savingManifest.key.world.c_str(), savingManifest.key.save.c_str(), savingManifest.key.player);
}

bool StartSave(char const* name)
{
    if (!pairedEnabled || !SafeName(name) || saving::Prefix(name, "G3T_") || barrier.phase != saving::Barrier::Idle || !StableRunningHero() ||
        !originalSave || !originalLoad || !originalWorldSave || !originalWorldLoad || !writing || !NativeSaveAllowed()) {
        Logf("persist: save request refused slot=%s (unsafe/reserved name, busy or no own stable hero)", name ? name : "-");
        Notice(i18n::SaveFailed, true);
        return false;
    }
    savingManifest = Manifest{};
    savingManifest.key = {current.key.world.empty() ? RandomId() : current.key.world, RandomId(), steam::PlayerId()};
    if (!saving::Valid(savingManifest.key)) {
        Logf("persist: checkpoint identity unavailable world=%s save=%s player=%llu; paired saves off for this save",
             savingManifest.key.world.c_str(), savingManifest.key.save.c_str(), steam::PlayerId());
        Notice(i18n::SaveFailed, true);
        return false;
    }
    barrier = saving::Barrier{};
    barrier.phase = saving::Barrier::Preparing;
    barrier.save = savingManifest.key.save;
    barrier.epoch = epoch;
    requestedSlot = name;
    saveStarted = GetTickCount();
    nativeReturnedAt = 0;
    aliasPublished = false;
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        if (!net::PeerJoined(i)) continue;
        if (!net::InWorld(i)) { CancelSave("a connected player is loading or outside this world"); return false; }
        barrier.members[i].generation = net::PeerGeneration(i);
        net::SaveNote prepare = Note(net::SavePrepare, savingManifest);
        strncpy_s(prepare.name, requestedSlot.c_str(), _TRUNCATE);
        if (!net::SendSave(i, prepare)) { CancelSave("prepare could not be sent"); return false; }
    }
    saveWindow = eCApplication::GetInstance().GetHandle();
    if (!saveWindow || !SetTimer(saveWindow, SaveTimer, 50, &SaveNow)) { CancelSave("save timer could not be armed"); return false; }
    Logf("persist: checkpoint requested world=%s save=%s player=%llu slot=%s epoch=%08x",
         savingManifest.key.world.c_str(), savingManifest.key.save.c_str(), steam::PlayerId(), name, epoch);
    Notice(i18n::SaveQueued);
    return true;
}

bool __stdcall SaveDetour(void* self, bCString const* name)
{
    if (!pairedEnabled) return originalSave && originalSave(self, name);
    if (!name || !SafeName(name->GetText())) {
        Log("persist: save refused; the name is not a SafeName");
        Notice(i18n::SaveFailed, true);
        return false;
    }
    if (nativeLoadDepth || nativeSaveDepth || !trade::ReadyForSave()) {
        Log("persist: save refused while loading/saving or a player exchange is unsettled");
        Notice(i18n::TradeSettling, true);
        return false;
    }
    if (net::IsHost()) StartSave(name->GetText());
    else if (!net::Joined() || !net::InWorld(net::LocalId()) || !StableRunningHero() || !NativeSaveAllowed() || requestPending || prepared.valid) {
        Log("persist: client save refused; no ready authoritative host/checkpoint or request already pending");
        Notice(i18n::SaveFailed, true);
    } else {
        net::SaveNote note{};
        note.op = net::SaveRequest;
        note.epoch = hostWorld.epoch;
        requestId = RandomId();
        if (requestId.empty()) { Notice(i18n::SaveFailed, true); return false; }
        strncpy_s(note.save, requestId.c_str(), _TRUNCATE);
        const std::string world = saving::Id(current.key.world) ? current.key.world : std::string(32, '0');
        strncpy_s(note.world, world.c_str(), _TRUNCATE);
        strncpy_s(note.name, name->GetText(), _TRUNCATE);
        if (net::SendSave(1, note)) {
            requestAt = GetTickCount();
            requestWelcome = net::WelcomeSerial();
            requestPending = true;
            Logf("persist: client requested authoritative world save slot=%s epoch=%08x player=%llu",
                 note.name, note.epoch, steam::PlayerId());
            Notice(i18n::SaveQueued);
        } else Notice(i18n::SaveFailed, true);
    }
    // The engine has not saved yet. Do not return success for a queued distributed operation.
    return false;
}

bool __fastcall WorldSaveDetour(gCWorld* self, void*, bCString const& name, eCGfxImageData* image, void* metadata, GEU32 size)
{
    // HUD/manual/quicksave reaches this only under our native-save scope. Other native callers join the same protocol.
    if (!pairedEnabled) return originalWorldSave(self, name, image, metadata, size);
    if (nativeSaveDepth) {
        Logf("persist: native invoke=World.SaveGameWorld save=%s player=%llu metadata_size=%lu",
             savingManifest.key.save.c_str(), steam::PlayerId(), size);
        const bool ok = originalWorldSave(self, name, image, metadata, size);
        Logf("persist: native result=World.SaveGameWorld save=%s player=%llu ok=%d metadata_size=%lu",
             savingManifest.key.save.c_str(), steam::PlayerId(), ok ? 1 : 0, size);
        return ok;
    }
    return SaveDetour(nullptr, &name);
}
bool __fastcall WorldLoadDetour(gCWorld* self, void*, bCString const& name, bool nativeFlag)
{
    const bool observed = nativeLoadDepth != 0;
    if (pairedEnabled && !observed) {
        Logf("persist: load refused function=World.LoadGameWorld slot=%s player=%llu; paired saves off for this load, use HUD.LoadGame",
             name.GetText(), steam::PlayerId());
        Notice(i18n::SaveFailed, true);
        return false;
    }
    if (observed) ++lowerLoadCalls;
    if (observed) Logf("persist: native invoke=World.LoadGameWorld save=%s player=%llu flag=%d",
                       current.key.save.c_str(), steam::PlayerId(), nativeFlag ? 1 : 0);
    const bool ok = originalWorldLoad(self, name, nativeFlag);
    if (observed) lowerLoadGood = lowerLoadGood && ok;
    if (observed) Logf("persist: native result=World.LoadGameWorld save=%s player=%llu ok=%d",
                       current.key.save.c_str(), steam::PlayerId(), ok ? 1 : 0);
    return ok;
}

bool CampaignComplete(std::string const& slot)
{
    static std::string observed;
    static WIN32_FILE_ATTRIBUTE_DATA seen[2]{};
    static uint32_t since = 0;
    WIN32_FILE_ATTRIBUTE_DATA now[2]{};
    for (int f = 0; f < 2; ++f) {
        if (!GetFileAttributesExA(Path(slot, f).c_str(), GetFileExInfoStandard, &now[f]) || now[f].nFileSizeHigh || !now[f].nFileSizeLow) return false;
    }
    bool changed = observed != slot;
    for (int f = 0; f < 2; ++f)
        if (seen[f].nFileSizeLow != now[f].nFileSizeLow || CompareFileTime(&seen[f].ftLastWriteTime, &now[f].ftLastWriteTime)) changed = true;
    if (changed) {
        observed = slot;
        std::copy(std::begin(now), std::end(now), std::begin(seen));
        since = GetTickCount();
    }
    return !changed && GetTickCount() - since >= 1000;
}

void PollSave()
{
    if (barrier.phase == saving::Barrier::Idle) return;
    if (barrier.epoch != epoch) { CancelSave("world changed"); return; }
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        if (!barrier.members[i].generation) continue;
        if (!net::PeerJoined(i) || net::PeerGeneration(i) != barrier.members[i].generation) {
            CancelSave("a participating player disconnected or rejoined");
            return;
        }
    }
    const uint32_t now = GetTickCount();
    if (barrier.phase == saving::Barrier::Committing) {
        for (uint8_t i = 2; i < net::MaxPlayers; ++i)
            if (barrier.members[i].generation && net::FileTransferBusy(i)) saveStarted = now;
    }
    if ((barrier.phase == saving::Barrier::Preparing && now - saveStarted > PrepareLimit) ||
        (barrier.phase == saving::Barrier::Writing && now - nativeReturnedAt > WriteLimit) ||
        (barrier.phase == saving::Barrier::Committing && now - saveStarted > TransferLimit)) {
        CancelSave("checkpoint phase timed out; any published host checkpoint is still recoverable");
        return;
    }
    if (barrier.phase == saving::Barrier::Preparing) return; // native call is exclusively in SaveNow
    if (barrier.phase == saving::Barrier::Committing) {
        if (barrier.AllStored()) FinishSave(aliasPublished, aliasPublished ? "all participant stores acknowledged" : "host alias failed");
        return;
    }
    const std::string slot = saving::Slot(barrier.save);
    if (now - nativeReturnedAt < 2000 || !CampaignComplete(slot)) return;
    std::vector<uint8_t> data[net::FileCount];
    for (int f = 0; f < 2; ++f) if (!ReadAll(Path(slot, f), data[f]) || data[f].empty()) return;
    for (int f = 0; f < 2; ++f) {
        savingManifest.size[f] = static_cast<uint32_t>(data[f].size());
        savingManifest.crc[f] = Crc32(data[f].data(), data[f].size());
    }
    data[2] = ManifestBytes(savingManifest, savedJournal);
    if (!WriteAtomic(Path(slot, 2), data[2])) { CancelSave("world manifest publication failed"); return; }
    ++sidecarsWritten;
    if (!PublishLatest(savingManifest.key)) { CancelSave("host character latest publication failed"); return; }
    campaign = slot;
    current = savingManifest;
    localKind = net::KindSaved;
    localSave = slot;
    lastSave = requestedSlot;
    aliasPublished = StoreBundle(requestedSlot, data);
    barrier.phase = saving::Barrier::Committing;
    saveStarted = now;
    for (uint8_t i = 2; i < net::MaxPlayers; ++i)
        if (barrier.members[i].generation && !SendCampaign(i, true)) { CancelSave("checkpoint transfer could not be queued"); return; }
    Logf("persist: host checkpoint published world=%s save=%s player=%llu alias=%s alias_ok=%d size=%u/%u/%zu crc=%08x/%08x/%08x",
         current.key.world.c_str(), current.key.save.c_str(), current.key.player, requestedSlot.c_str(), aliasPublished ? 1 : 0,
         current.size[0], current.size[1], data[2].size(), current.crc[0], current.crc[1], Crc32(data[2].data(), data[2].size()));
}

void OnSave(uint8_t from, net::SaveNote const& note)
{
    if (!pairedEnabled) {
        if (note.op == net::SaveRequest || note.op == net::SavePrepare) {
            net::SaveNote answer = note;
            answer.op = net::IsHost() ? net::SaveResult : net::SaveReady;
            answer.ok = 0;
            net::SendSave(net::IsHost() ? from : 1, answer);
            Logf("persist: checkpoint refused save=%s player=%llu; paired saves off", note.save, steam::PlayerId());
        }
        return;
    }
    if (net::IsHost()) {
        if (note.epoch != epoch || from < 2 || from >= net::MaxPlayers) return;
        if (note.op == net::SaveRequest) {
            if (!saving::Id(note.world) || !saving::Id(note.save)) return;
            if (!net::InWorld(from) || !SafeName(note.name) || !StartSave(note.name)) {
                net::SaveNote answer = note;
                answer.op = net::SaveResult;
                answer.ok = 0;
                net::SendSave(from, answer);
            }
        } else if (barrier.Matches(note.save, note.epoch) && note.world == savingManifest.key.world) {
            if (barrier.members[from].generation != net::PeerGeneration(from) ||
                !barrier.members[from].generation ||
                (note.op == net::SaveReady && barrier.phase != saving::Barrier::Preparing) ||
                (note.op == net::SaveStored && barrier.phase != saving::Barrier::Committing)) return;
            if (!note.ok) { CancelSave("a participant refused or failed its character/checkpoint store"); return; }
            if (note.op == net::SaveReady) barrier.Ready(from, net::PeerGeneration(from), note.save, note.epoch);
            else if (note.op == net::SaveStored) barrier.Stored(from, net::PeerGeneration(from), note.save, note.epoch);
            Logf("persist: checkpoint ack=%s save=%s player_slot=%u generation=%u ok=%u",
                 note.op == net::SaveReady ? "ready" : "stored", note.save, from, net::PeerGeneration(from), note.ok);
        }
        return;
    }
    if (note.epoch != hostWorld.epoch) return;
    if (note.op == net::SavePrepare) {
        const saving::Key key{note.world, note.save, steam::PlayerId()};
        const saving::Key captured{prepared.note.world, prepared.note.save, steam::PlayerId()};
        if (prepared.valid && saving::PreparedMatches(captured, key, prepared.welcome, net::WelcomeSerial(),
                                                     prepared.note.epoch, hostWorld.epoch, prepared.note.name, note.name)) {
            net::SaveNote answer = note;
            answer.op = net::SaveReady;
            net::SendSave(1, answer);
            return;
        }
        const bool ok = !prepared.valid && SafeName(note.name) && !saving::Prefix(note.name, "G3T_") &&
                        net::InWorld(net::LocalId()) && StableRunningHero() && NativeSaveAllowed() &&
                        saving::Valid(key) && CaptureCharacter(key);
        if (ok) {
            prepared = {note, net::WelcomeSerial(), GetTickCount(), true};
            ++recordsWritten;
            requestPending = false;
        }
        net::SaveNote answer = note;
        answer.op = net::SaveReady;
        answer.ok = ok ? 1 : 0;
        net::SendSave(1, answer);
        Logf("persist: checkpoint character prepare world=%s save=%s player=%llu ok=%d", note.world, note.save, steam::PlayerId(), ok ? 1 : 0);
        if (!ok) Notice(i18n::SaveFailed, true);
    } else if (note.op == net::SaveCommit || note.op == net::SaveImport) {
        if (!SafeName(note.name) || note.name != saving::Slot(note.save)) return;
        if (note.op == net::SaveCommit && (!prepared.valid || prepared.welcome != net::WelcomeSerial() ||
            std::strcmp(prepared.note.save, note.save) || std::strcmp(prepared.note.world, note.world))) return;
        ClearIncoming();
        incomingNote = note;
        incomingWelcome = net::WelcomeSerial();
        receiving = true;
        incomingAt = GetTickCount();
        Logf("persist: checkpoint receive begin save=%s player=%llu mode=%s epoch=%08x",
             note.save, steam::PlayerId(), note.op == net::SaveCommit ? "store" : "load", note.epoch);
    } else if (note.op == net::SaveCancel || note.op == net::SaveResult) {
        const bool relevant = prepared.valid ? !std::strcmp(prepared.note.save, note.save) :
                              (requestPending && requestId == note.save) || current.key.save == note.save;
        if (!relevant) return;
        prepared = Prepared{};
        requestPending = false;
        if (!note.ok && receiving && !std::strcmp(incomingNote.save, note.save)) ClearIncoming();
        Logf("persist: host checkpoint result world=%s save=%s player=%llu ok=%u", note.world, note.save, steam::PlayerId(), note.ok);
        Notice(note.ok ? i18n::SavePaired : i18n::SaveFailed, !note.ok);
    }
}

char const* KindName(uint8_t kind) { return kind == net::KindNew ? "new-game" : kind == net::KindSaved ? "save" : "menu"; }
void SendAnswer(uint8_t op)
{
    net::WorldNote note{};
    note.op = op;
    note.kind = localKind;
    note.epoch = hostWorld.epoch;
    strncpy_s(note.campaign, localSave.c_str(), _TRUNCATE);
    net::SendWorld(note, 1);
    ++(op == net::WorldReady ? readies : others);
}
void Evaluate()
{
    if (!net::Joined() || !hostWorld.epoch || nativeLoadDepth || !loading.empty() || newWorld) return;
    if (!pendingRecord.empty() || !refusedRecord.empty() || !waitingImport.empty() || !ownCharacterReady) {
        net::SetInWorld(net::LocalId(), false);
        SendAnswer(net::WorldWaiting);
        return;
    }
    const bool matchingSave = localKind == net::KindSaved && hostWorld.kind == net::KindSaved &&
                              localSave == hostWorld.campaign && localSave == saving::Slot(current.key.save);
    const bool freshNewWorld = hostWorld.kind == net::KindNew && localKind == net::KindNew &&
                               (!everIn || loadedEpoch == hostWorld.epoch);
    if (saving::WorldMatches(hostWorld.epoch, synced, loadedEpoch, matchingSave, freshNewWorld)) {
        synced = hostWorld.epoch;
        everIn = true;
        net::SetInWorld(net::LocalId(), true);
        SendAnswer(net::WorldReady);
        return;
    }
    net::SetInWorld(net::LocalId(), false);
    SendAnswer(net::WorldOther);
    if (hinted != hostWorld.epoch) {
        hinted = hostWorld.epoch;
        Notice(hostWorld.kind == net::KindMenu ? i18n::HostInMenu : hostWorld.kind == net::KindNew ? i18n::HostNewGame :
               everIn ? i18n::HostLoadedSave : i18n::LoadingHostSave);
    }
}
void Tell(uint8_t to)
{
    net::WorldNote note{};
    note.op = net::WorldHost;
    note.kind = localKind == net::KindMenu ? net::KindMenu : campaign.empty() ? net::KindNew : net::KindSaved;
    note.flags = syncing ? 0 : net::WorldSyncOff;
    note.epoch = epoch;
    strncpy_s(note.campaign, campaign.c_str(), _TRUNCATE);
    net::SendWorld(note, to);
    clients[to].told = epoch;
    clients[to].toldCampaign = campaign;
}
void NewEpoch()
{
    CancelSave("host world changed");
    net::CancelFileTransfers();
    static const uint32_t nonce = ((GetTickCount() * 2654435761u) ^ (GetCurrentProcessId() << 16)) & 0xffff0000u;
    epoch = nonce | (++epochs & 0xffff);
    if (!epoch) epoch = ++epochs;
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        clients[i].resends = 0;
        net::SetInWorld(i, false);
    }
    Logf("persist: host world %08x: %s %s", epoch, KindName(localKind), campaign.c_str());
}
void Changing(uint8_t kind)
{
    localKind = kind;
    localSave.clear();
    ++worldChanges;
    if (net::IsHost()) {
        if (kind == net::KindMenu) { campaign.clear(); current = Manifest{}; }
        NewEpoch();
    } else {
        synced = 0;
        loadedEpoch = 0;
        prepared = Prepared{};
        requestPending = false;
        ClearIncoming();
        refusedRecord.clear();
        net::SetInWorld(net::LocalId(), false);
    }
}
void Settled(uint8_t kind, std::string const& save)
{
    localKind = kind;
    localSave = kind == net::KindSaved && saving::Valid(current.key) ? saving::Slot(current.key.save) : save;
    settledAt = GetTickCount();
    if (!net::IsHost()) loadedEpoch = hostWorld.epoch;
    if (kind == net::KindNew) ownCharacterReady = true;
    Logf("persist: world settled kind=%s slot=%s world=%s save=%s player=%llu own=%d", KindName(kind), localSave.c_str(),
         current.key.world.c_str(), current.key.save.c_str(), steam::PlayerId(), ownCharacterReady ? 1 : 0);
    if (net::IsHost()) {
        if (kind == net::KindNew) { campaign.clear(); current = Manifest{}; current.key.world = RandomId(); NewEpoch(); }
    } else Evaluate();
}
void OnWorld(uint8_t from, net::WorldNote const& note)
{
    if (!net::IsHost()) {
        if (note.op != net::WorldHost) return;
        if (note.epoch != hostWorld.epoch) {
            prepared = Prepared{};
            requestPending = false;
            ClearIncoming();
            waitingImport.clear();
            pendingRecord.clear();
            refusedRecord.clear();
            synced = 0;
            net::SetInWorld(net::LocalId(), false);
        }
        hostWorld = note;
        Evaluate();
        return;
    }
    if (from < 2 || from >= net::MaxPlayers || note.epoch != epoch) return;
    Client& client = clients[from];
    if (note.op == net::WorldReady) {
        client.ready = epoch;
        if (!nativeLoadDepth && ownCharacterReady && pendingRecord.empty() && refusedRecord.empty())
            net::SetInWorld(from, true);
        client.arrived = true;
    } else if (note.op == net::WorldOther || note.op == net::WorldWaiting) {
        client.ready = 0;
        net::SetInWorld(from, false);
        const bool onWay = net::FileTransferBusy(from) ||
            (resendGuard && client.sentAt && !client.arrived && GetTickCount() - client.sentAt < TransferLimit);
        if (!campaign.empty() && !onWay && transferring && (client.sent != epoch || client.resends < 2)) {
            if (client.sent == epoch) ++client.resends;
            client.sent = epoch;
            client.wants = true;
        }
        if (note.op == net::WorldWaiting && client.hinted != epoch) {
            client.hinted = epoch;
            Logf("persist: player %u (%s) waits for its own character in world %08x", from, net::PeerName(from), epoch);
        }
    }
}
void HostTick()
{
    if ((localKind != net::KindMenu && !ownCharacterReady) || !loading.empty() ||
        !pendingRecord.empty() || !refusedRecord.empty()) {
        for (uint8_t i = 2; i < net::MaxPlayers; ++i) net::SetInWorld(i, false);
        return;
    }
    if (!epoch) NewEpoch();
    if (static_cast<int32_t>(pausedUntil - GetTickCount()) > 0) return;
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        Client& client = clients[i];
        if (!net::PeerJoined(i)) { client = Client{}; continue; }
        if (client.generation != net::PeerGeneration(i)) {
            client = Client{};
            client.generation = net::PeerGeneration(i);
        }
        if (client.told != epoch || client.toldCampaign != campaign) Tell(i);
        if (client.ready == epoch && !nativeLoadDepth) net::SetInWorld(i, true);
        if (client.wants && barrier.phase == saving::Barrier::Idle) {
            if (client.ready == epoch || SendCampaign(i)) client.wants = false;
        }
    }
}

void DropAutoload(char const* why)
{
    if (!autoload.valid) return;
    if (autoload.armed && autoload.window) KillTimer(autoload.window, AutoloadTimer);
    ++autoloadStale;
    Logf("persist: autoload canceled slot=%s order=%u reason=%s", autoload.slot.c_str(), autoload.serial, why);
    autoload = Autoload{};
}
void CALLBACK AutoloadNow(HWND window, UINT, UINT_PTR id, DWORD)
{
    if (!autoload.valid || !autoload.armed) { KillTimer(window, id); return; }
    if (processDepth || nativeLoadDepth || nativeSaveDepth) return;
    if (net::IsHost() || !net::Joined() || autoload.welcome != net::WelcomeSerial() ||
        autoload.epoch != hostWorld.epoch || autoload.loads != loads) { DropAutoload("world/welcome/load changed"); return; }
    if (autoloadHold) return;
    const std::string slot = autoload.slot;
    KillTimer(window, id);
    autoload = Autoload{};
    void* manager = FileManager(true);
    if (!manager || !originalLoad) {
        Logf("persist: autoload preflight failed save=%s player=%llu; paired load not invoked", slot.c_str(), steam::PlayerId());
        Notice(i18n::SaveFailed, true);
        return;
    }
    const bCString name(slot.c_str());
    ++autoLoads;
    LoadDetour(manager, &name);
}
bool ArmAutoload()
{
    HWND window = eCApplication::GetInstance().GetHandle();
    if (!window || !SetTimer(window, AutoloadTimer, USER_TIMER_MINIMUM, &AutoloadNow)) {
        Logf("persist: autoload timer unavailable slot=%s error=%lu", autoload.slot.c_str(), GetLastError());
        return false;
    }
    autoload.window = window;
    autoload.armed = true;
    ++autoloadPosted;
    return true;
}
void OrderAutoload(std::string const& slot)
{
    DropAutoload("new import");
    autoload = {slot, hostWorld.epoch, net::WelcomeSerial(), loads, ++autoloadSerial, nullptr, true, false};
    ArmAutoload();
}

bool __stdcall LoadDetour(void* self, bCString const* name)
{
    if (!pairedEnabled) return originalLoad && originalLoad(self, name);
    if (!name || !SafeName(name->GetText()) || nativeLoadDepth || nativeSaveDepth || !trade::ReadyForSave()) {
        Log("persist: load refused (unsafe name, nested native operation or unsettled exchange)");
        Notice(i18n::SaveFailed, true);
        return false;
    }
    CancelSave("a load was requested");
    if (self != FileManager(true)) {
        Logf("persist: native load preflight failed player=%llu; paired load not invoked", steam::PlayerId());
        Notice(i18n::SaveFailed, true);
        return false;
    }
    const std::string slot = name->GetText();
    Manifest manifest;
    std::vector<uint8_t> data[net::FileCount];
    bool managed = ManifestAt(slot, manifest);
    if (managed && !ReadBundle(slot, manifest, data)) {
        Logf("persist: checkpoint native CRC/length mismatch slot=%s; not loaded", slot.c_str());
        Notice(i18n::SaveDamaged, true);
        return false;
    }
    if (!managed && saving::Prefix(slot, "G3T_") && !saving::Prefix(slot, "G3T_host_")) {
        Logf("persist: managed checkpoint has no valid manifest slot=%s; not loaded", slot.c_str());
        Notice(i18n::SaveDamaged, true);
        return false;
    }
    if (!managed && Exists(Path(slot, 2))) {
        std::vector<uint8_t> bytes;
        if (!ReadAll(Path(slot, 2), bytes) || std::string(bytes.begin(), bytes.end()).compare(0, 5, "G3T 2") == 0) {
            Logf("persist: malformed modern checkpoint manifest slot=%s", slot.c_str());
            Notice(i18n::SaveDamaged, true);
            return false;
        }
    }
    if (!managed && (net::IsHost() || saving::Prefix(slot, "G3T_host_"))) {
        if (!AdoptLegacy(slot, manifest)) { Logf("persist: legacy adoption failed slot=%s", slot.c_str()); Notice(i18n::SaveFailed, true); return false; }
        managed = true;
        if (!ReadBundle(saving::Slot(manifest.key.save), manifest, data)) {
            Logf("persist: adopted legacy bundle could not be verified slot=%s", slot.c_str());
            Notice(i18n::SaveDamaged, true);
            return false;
        }
    }
    if (managed && net::IsHost()) {
        const std::string canonical = saving::Slot(manifest.key.save);
        Manifest stored;
        std::vector<uint8_t> canonicalData[net::FileCount];
        const bool validCanonical = ReadBundle(canonical, stored, canonicalData) &&
            saving::Exact(stored.key, manifest.key) && stored.size[0] == manifest.size[0] &&
            stored.size[1] == manifest.size[1] && stored.crc[0] == manifest.crc[0] && stored.crc[1] == manifest.crc[1] &&
            canonicalData[2] == data[2];
        if (!validCanonical) {
            if (!StoreBundle(canonical, data)) {
                Logf("persist: canonical bundle repair failed slot=%s canonical=%s", slot.c_str(), canonical.c_str());
                Notice(i18n::SaveFailed, true);
                return false;
            }
            Logf("persist: canonical bundle repaired from verified alias slot=%s canonical=%s", slot.c_str(), canonical.c_str());
        }
    }
    pendingRecord.clear();
    refusedRecord.clear();
    const bool overlay = managed && (!net::IsHost() || manifest.key.player != steam::PlayerId());
    if (overlay && !ChooseCharacter(slot, manifest)) {
        if (!net::IsHost()) {
            waitingImport = slot;
            waitingEpoch = hostWorld.epoch;
            waitingWelcome = net::WelcomeSerial();
            nextImportTry = GetTickCount() + 2000;
            net::SetInWorld(net::LocalId(), false);
        }
        return false;
    }
    const bool loadingHostCheckpoint = managed && saving::Slot(manifest.key.save) == hostWorld.campaign;
    const std::string deferredImport = !net::IsHost() && !loadingHostCheckpoint && net::Joined() &&
        waitingEpoch == hostWorld.epoch && waitingWelcome == net::WelcomeSerial() ? waitingImport : std::string();
    ++loads;
    DropAutoload("load began");
    waitingImport = deferredImport;
    const std::string record = pendingRecord;
    const saving::Key selected = pendingKey, desired = desiredKey;
    const bool legacy = pendingLegacy;
    lastLoad = slot;
    loading = slot;
    Changing(net::KindSaved);
    pendingRecord = record;
    pendingKey = selected;
    desiredKey = desired;
    pendingLegacy = legacy;
    current = manifest;
    ownCharacterReady = !overlay;
    if (net::IsHost()) {
        campaign = managed ? saving::Slot(manifest.key.save) : slot;
    }
    FILE* journalFile = nullptr;
    bool journalRead = false;
    bool savedDowned = false;
    if (!fopen_s(&journalFile, Path(slot, 2).c_str(), "r") && journalFile) {
        char header[32]{};
        if (fgets(header, sizeof(header), journalFile) && (!std::strncmp(header, "G3T 1", 5) || !std::strncmp(header, "G3T 2", 5))) {
            journal::Read(journalFile);
            if (net::IsHost()) {
                fseek(journalFile, 0, SEEK_SET);
                npcs::ReadCompanions(journalFile);
                rewind(journalFile);
                char line[256];
                while (fgets(line, sizeof(line), journalFile)) {
                    if (std::strncmp(line, "downed ", 7)) continue;
                    int waiting = 0;
                    char extra = 0;
                    if (sscanf_s(line, "downed %d %c", &waiting, &extra, 1u) == 1 && (waiting == 0 || waiting == 1))
                        savedDowned = waiting != 0;
                    else Log("downed: invalid host-save state; current downed life retained");
                }
            }
            journalRead = true;
            ++sidecarsRead;
        }
        fclose(journalFile);
    }
    if (!journalRead) { journal::Clear(); if (net::IsHost()) npcs::ClearCompanions(); }
    Logf("persist: loading slot=%s world=%s save=%s native_owner=%llu player=%llu overlay=%s",
         slot.c_str(), manifest.key.world.c_str(), manifest.key.save.c_str(), manifest.key.player, steam::PlayerId(),
         pendingRecord.empty() ? "native-own" : pendingRecord.c_str());
    dialogaudio::OnWorldChange();
    containers::OnWorldChange();
    world::OnWorldDeactivate();
    lowerLoadCalls = 0;
    lowerLoadGood = true;
    Logf("persist: native invoke=HUD.LoadGame world=%s save=%s player=%llu slot=%s size=%u/%u crc=%08x/%08x",
         manifest.key.world.c_str(), manifest.key.save.c_str(), steam::PlayerId(), slot.c_str(),
         manifest.size[0], manifest.size[1], manifest.crc[0], manifest.crc[1]);
    ++nativeLoadDepth;
    const bool hudOk = originalLoad(self, name);
    --nativeLoadDepth;
    const bool ok = hudOk && lowerLoadCalls == 1 && lowerLoadGood;
    if (ok && net::IsHost()) downed::Restore(savedDowned, "host-save");
    Logf("persist: native load returned function=HUD.LoadGame slot=%s save=%s player=%llu ok=%d hud_ok=%d lower_calls=%u lower_ok=%d frames=%u",
         slot.c_str(), manifest.key.save.c_str(), steam::PlayerId(), ok ? 1 : 0, hudOk ? 1 : 0,
         lowerLoadCalls, lowerLoadGood ? 1 : 0, loadFrames);
    if (!ok) {
        loading.clear();
        pendingRecord.clear();
        ownCharacterReady = false;
        if (net::IsHost()) {
            current = Manifest{};
            campaign.clear();
            localKind = net::KindMenu;
            NewEpoch();
        }
        Notice(i18n::SaveFailed, true);
    }
    return ok;
}

FILETIME WriteTime(std::string const& path)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info);
    return info.ftLastWriteTime;
}
enum class ApplyResult { Restored, Refused, PublicationPending };
ApplyResult ApplyCharacter()
{
    const bool ok = pendingLegacy ? hero::LoadLegacyRecordStrict(pendingRecord.c_str()) :
                                   hero::LoadRecord(pendingRecord.c_str(), pendingKey);
    if (!CharacterDigest("restore", pendingKey, pendingRecord, ok)) return ApplyResult::Refused;
    const std::string exact = CharacterPath(desiredKey);
    if (pendingLegacy || !saving::Exact(pendingKey, desiredKey)) {
        if (!CaptureCharacter(desiredKey)) return ApplyResult::PublicationPending;
        ++recordsWritten;
    }
    if (!PublishLatest(desiredKey)) return ApplyResult::PublicationPending;
    ++recordsApplied;
    ownCharacterReady = true;
    Logf("persist: own character restored world=%s save=%s player=%llu source=%s legacy=%d",
         desiredKey.world.c_str(), desiredKey.save.c_str(), desiredKey.player, pendingRecord.c_str(), pendingLegacy ? 1 : 0);
    return ApplyResult::Restored;
}
void WaitForRecord()
{
    const uint32_t now = GetTickCount();
    const FILETIME written = WriteTime(refusedRecord);
    if ((CompareFileTime(&written, &refusedWritten) != 0 || static_cast<int32_t>(now - nextRestoreTry) >= 0) &&
        Exists(refusedRecord)) {
        refusedWritten = written;
        pendingRecord = refusedRecord;
        const ApplyResult result = ApplyCharacter();
        if (result == ApplyResult::Restored) {
            refusedRecord.clear();
            pendingRecord.clear();
            Notice(i18n::RecordBack);
            Evaluate();
            return;
        }
        nextRestoreTry = now + (result == ApplyResult::PublicationPending ? 5000 : 30000);
        Logf("persist: own character retry blocked world=%s save=%s player=%llu phase=%s",
             desiredKey.world.c_str(), desiredKey.save.c_str(), desiredKey.player,
             result == ApplyResult::PublicationPending ? "publication" : "validation/application");
        pendingRecord.clear();
    }
    if (!refusedHinted || now - refusedHinted >= 30000) {
        refusedHinted = now;
        Logf("persist: waiting for own character world=%s save=%s player=%llu record=%s; deletion never permits host fallback",
             desiredKey.world.c_str(), desiredKey.save.c_str(), desiredKey.player, refusedRecord.c_str());
        Notice(i18n::SaveCharacterMissing, true);
    }
}
void StatusHints()
{
    static bool wasJoined = false, everJoined = false, unreachableHinted = false, menuSet = false;
    static std::string shown;
    static std::wstring refusal;
    static uint32_t menuAt = 0, hintedAt = 0;
    static bool startupShown = false;
    Config const& config = GetConfig();
    const uint32_t now = GetTickCount();
    if (*net::StartupError()) {
        if (now - menuAt < 1000) return;
        menuAt = now;
        wchar_t text[500], reason[256] = {};
        MultiByteToWideChar(CP_UTF8, 0, net::StartupError(), -1, reason, 255);
        _snwprintf_s(text, _TRUNCATE, i18n::Text(config.host ? i18n::SteamUnavailableHost : i18n::SteamUnavailableClient), reason);
        if (!gCSession::GetInstance().IsGameRunning()) {
            if (hud::MenuLine(text)) menuSet = true;
        } else if (!startupShown) {
            startupShown = true;
            hud::Message(hud::Bad, text);
        }
        return;
    }
    const bool joined = !config.host && net::Joined();
    if (wasJoined && !joined) hud::Message(hud::Warning, i18n::Text(i18n::ConnectionLost));
    else if (!wasJoined && joined && everJoined) hud::Message(hud::Good, i18n::Text(i18n::Reconnected));
    everJoined = everJoined || joined;
    wasJoined = joined;
    char const* reason = net::RefusalReason();
    if (*reason && shown != reason) {
        shown = reason;
        static char const install[] = "install differs from the host: ";
        wchar_t text[400], wide[200] = {};
        unsigned mine = 0, host = 0;
        if (sscanf_s(reason, "different G3 Together version (protocol %u, host %u)", &mine, &host) == 2) {
            _snwprintf_s(text, _TRUNCATE, i18n::Text(i18n::RefusedVersion), mine, host);
        } else {
            char const* list = std::strstr(reason, install);
            MultiByteToWideChar(CP_UTF8, 0, list ? list + sizeof(install) - 1 : reason, -1, wide, 199);
            _snwprintf_s(text, _TRUNCATE, i18n::Text(list ? i18n::RefusedInstall : i18n::RefusedOther), wide);
        }
        hud::Message(hud::Bad, text);
        refusal = text;
    }
    if (now - menuAt < 1000) return;
    menuAt = now;
    const bool running = gCSession::GetInstance().IsGameRunning();
    const uint32_t trying = net::ConnectingFor();
    if (!running) {
        if (!refusal.empty()) {
            if (hud::MenuLine(refusal.c_str())) menuSet = true;
        } else if (joined && !waitingImport.empty()) {
            if (hud::MenuLine(i18n::Text(i18n::SaveCharacterMissing))) menuSet = true;
        } else {
            wchar_t text[700] = {};
            lobby::Format(lobby::Snapshot(), text, 700);
            if (hud::MenuLine(text)) menuSet = true;
        }
    }
    if (trying >= 15000 && refusal.empty()) {
        wchar_t text[400], hostName[80] = {};
        MultiByteToWideChar(CP_UTF8, 0, steam::HostName(), -1, hostName, 79);
        if (*hostName) _snwprintf_s(text, _TRUNCATE, i18n::Text(i18n::SteamConnecting), hostName, trying / 1000);
        else _snwprintf_s(text, _TRUNCATE, i18n::Text(i18n::SteamSearching), trying / 1000);
        if (!unreachableHinted)
            Logf("net: no host over Steam for %u s (%s); still trying (said in the menu and the game)", trying / 1000,
                 *steam::HostName() ? steam::HostName() : "no friend hosting");
        unreachableHinted = true;
        if (running && (!hintedAt || now - hintedAt >= 30000)) {
            hintedAt = now ? now : 1;
            hud::Message(hud::Warning, text);
        }
        return;
    }
    if (unreachableHinted && !trying) {
        unreachableHinted = false;
        Log("net: host reached after the not-reachable hint");
        if (running) hud::Message(hud::Good, i18n::Text(i18n::HostReached));
    }
}
}

bool BeginFrame()
{
    if (nativeLoadDepth || nativeSaveDepth) {
        ++loadFrames;
        if (!loadTicks) return false;
        ++loadFramesRan;
    }
    ++processDepth;
    return true;
}
void EndFrame() { if (processDepth) --processDepth; }
bool NativeLoading() { return nativeLoadDepth != 0; }
bool HeroPending()
{
    return pairedEnabled && (NativeLoading() || !loading.empty() || !pendingRecord.empty() || !refusedRecord.empty() ||
                             !waitingImport.empty() || !ownCharacterReady);
}
void OnWorldActivate()
{
    if (!loading.empty() || nativeSaveDepth) return;
    if (localKind == net::KindSaved && GetTickCount() - settledAt < 5000) return;
    newWorld = true;
    if (localKind != net::KindMenu) Changing(net::KindMenu);
}
void OnWorldDeactivate()
{
    if (!loading.empty() || nativeSaveDepth) return;
    ownCharacterReady = false;
    Changing(net::KindMenu);
}
void Tick()
{
    if (nativeLoadDepth || nativeSaveDepth) return;
    const uint32_t now = GetTickCount();
    StatusHints();
    gCSession& game = gCSession::GetInstance();
    if (game.GetHero() && game.IsGameRunning()) {
        if (!loading.empty()) {
            const std::string slot = loading;
            loading.clear();
            newWorld = false;
            Settled(net::KindSaved, slot);
        } else if (newWorld) { newWorld = false; Settled(net::KindNew, ""); }
    }
    if (net::IsHost()) { PollSave(); HostTick(); }
    else {
        if (receiving && (!net::Joined() || incomingWelcome != net::WelcomeSerial() ||
            incomingNote.epoch != hostWorld.epoch || now - incomingAt > TransferLimit)) {
            Logf("persist: checkpoint receive expired save=%s player=%llu mode=%s",
                 incomingNote.save, steam::PlayerId(), incomingNote.op == net::SaveCommit ? "store" : "load");
            ClearIncoming();
            Notice(i18n::SaveFailed, true);
        }
        if (!waitingImport.empty() && (!net::Joined() || waitingEpoch != hostWorld.epoch || waitingWelcome != net::WelcomeSerial())) {
            Log("persist: deferred host import canceled after world/welcome changed");
            waitingImport.clear();
        }
        if (prepared.valid && (!net::Joined() || prepared.welcome != net::WelcomeSerial() ||
            prepared.note.epoch != hostWorld.epoch || now - prepared.since > PreparedLimit)) {
            Logf("persist: prepared checkpoint expired save=%s player=%llu", prepared.note.save, steam::PlayerId());
            prepared = Prepared{};
            ClearIncoming();
            Notice(i18n::SaveFailed, true);
        }
        if (requestPending && (!net::Joined() || requestWelcome != net::WelcomeSerial() || now - requestAt > PrepareLimit)) {
            requestPending = false;
            Notice(i18n::SaveFailed, true);
        }
        if (autoload.valid) {
            if (!net::Joined() || autoload.welcome != net::WelcomeSerial() || autoload.epoch != hostWorld.epoch) DropAutoload("left host/world changed");
            else if (!autoload.armed) ArmAutoload();
        } else if (!waitingImport.empty() && pendingRecord.empty() && refusedRecord.empty() &&
                   static_cast<int32_t>(now - nextImportTry) >= 0) {
            nextImportTry = now + 2000;
            Manifest manifest;
            if (ManifestAt(waitingImport, manifest) && ChooseCharacter(waitingImport, manifest)) {
                pendingRecord.clear();
                OrderAutoload(waitingImport);
            }
        }
    }
    if (!refusedRecord.empty()) { WaitForRecord(); return; }
    if (pendingRecord.empty() || !game.GetHero() || !game.IsGameRunning()) return;
    if (!pendingSince) { pendingSince = now; return; }
    if (now - pendingSince < 3000) return;
    const ApplyResult result = ApplyCharacter();
    if (result != ApplyResult::Restored) {
        refusedRecord = pendingRecord;
        refusedWritten = WriteTime(refusedRecord);
        refusedHinted = 0;
        nextRestoreTry = now + (result == ApplyResult::PublicationPending ? 5000 : 30000);
        ownCharacterReady = false;
        Logf("persist: character refused world=%s save=%s player=%llu reason=%s", desiredKey.world.c_str(), desiredKey.save.c_str(),
             desiredKey.player, result == ApplyResult::PublicationPending ? "record publication failed; will retry" :
             (hero::LastRefusal() ? hero::LastRefusal() : "record application failed"));
    }
    pendingRecord.clear();
    Evaluate();
}
void Install()
{
    if (installed) return;
    net::SetSaveHandler(&OnSave);
    net::SetFileHandlers(&OnFileChunk, &OnFileDone);
    net::SetWorldHandler(&OnWorld);
    unsigned char* resolved[5]{};
    if (!ResolveNative(resolved)) {
        Log("persist: save/load hooks not installed; paired saves off");
        installed = true;
        return;
    }
    originalSave = reinterpret_cast<FileFn>(ai::JumpTo(resolved[0], reinterpret_cast<void*>(&SaveDetour), Stolen));
    originalLoad = reinterpret_cast<FileFn>(ai::JumpTo(resolved[1], reinterpret_cast<void*>(&LoadDetour), Stolen));
    // Six whole, position-independent bytes: push ebp / mov ebp,esp / and esp,-8.
    originalWorldSave = reinterpret_cast<WorldSaveFn>(ai::JumpTo(resolved[2], reinterpret_cast<void*>(&WorldSaveDetour), Stolen));
    originalWorldLoad = reinterpret_cast<WorldLoadFn>(ai::JumpTo(resolved[3], reinterpret_cast<void*>(&WorldLoadDetour), Stolen));
    pairedEnabled = originalSave && originalLoad && originalWorldSave && originalWorldLoad;
    if (originalSave) saveTarget = resolved[0];
    if (originalLoad) loadTarget = resolved[1];
    installed = true;
    if (!pairedEnabled) {
        Logf("persist: save/load hook installation failed SaveGameFn=%d LoadGameFn=%d WorldSaveGameFn=%d WorldLoadGameFn=%d; partial hooks pass through, paired saves off",
             originalSave != nullptr, originalLoad != nullptr, originalWorldSave != nullptr, originalWorldLoad != nullptr);
        return;
    }
    Logf("persist: save and load hooked SaveGameFn=%p LoadGameFn=%p WorldSaveGameFn=%p WorldLoadGameFn=%p FileManagerGetter=%p protocol=%u",
         resolved[0], resolved[1], resolved[2], resolved[3], resolved[4], saving::Protocol);
}
void* SaveTarget() { return saveTarget; }
void* LoadTarget() { return loadTarget; }
bool CheckpointBusy()
{
    return barrier.phase != saving::Barrier::Idle || prepared.valid || requestPending || nativeSaveDepth;
}

// Keep existing diagnostic entry points; negative controls may not reintroduce a host-character fallback.
void Off(LogFn log) { writing = false; log("PERSISTOFF ok=1"); }
void CampaignOff(LogFn log) { transferring = false; log("CAMPAIGNOFF ok=1"); }
void WorldSyncOff(LogFn log) { syncing = false; log("WORLDSYNCOFF ok=1"); }
void GenerationOff(LogFn log) { byGeneration = false; log("GENERATIONOFF retired=1"); }
void WorldGateLate(LogFn log) { earlyGate = false; log("WORLDGATELATE retired=1"); }
void RecordWaitOff(LogFn log) { log("RECORDWAITOFF retired=1 (own character is mandatory)"); }
void Pause(char const* argument, LogFn log)
{
    const int ms = std::atoi(argument);
    if (net::IsHost() && ms > 0 && ms <= 30000) pausedUntil = GetTickCount() + static_cast<uint32_t>(ms);
    log("PERSISTPAUSE");
}
void BadTransfer(LogFn log)
{
    if (!net::IsHost()) return;
    uint8_t junk[32]{};
    uint32_t sizes[net::FileCount] = {100, 100, 0}, crcs[net::FileCount]{};
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) if (net::PeerJoined(i)) {
        net::SendFileChunk(i, 0, 100, 0xfffffff0u, junk, sizeof(junk));
        net::SendFileDone(i, "..\\..\\G3T_evil", sizes, crcs);
    }
    log("BADTRANSFER");
}
void Waiting(LogFn log)
{
    char line[400];
    Format(line, "RECORDWAIT waiting=%d record=%.300s", !refusedRecord.empty() || !waitingImport.empty(), refusedRecord.c_str());
    log(line);
}
void LoadTicksOff(char const* argument, LogFn log)
{
    if (argument && !std::strcmp(argument, "hold")) autoloadHold = true;
    else if (argument && !std::strcmp(argument, "release")) autoloadHold = false;
    else if (argument && !std::strcmp(argument, "welcome")) net::AdvanceWelcomeSerialForTest();
    else loadTicks = true;
    log("LOADTICKSOFF");
}
void WorldReport(LogFn log)
{
    char line[700];
    Format(line, "WORLD role=%s kind=%s save=%s epoch=%08x host_epoch=%08x synced=%08x own=%d world_id=%s save_id=%s",
         net::IsHost() ? "host" : "client", KindName(localKind), localSave.c_str(), epoch, hostWorld.epoch, synced,
         ownCharacterReady ? 1 : 0, current.key.world.c_str(), current.key.save.c_str());
    log(line);
}
void Report(LogFn log)
{
    char line[900];
    Format(line, "PERSIST hooked=%d saves=%u loads=%u written=%u read=%u last_save=%s last_load=%s records_written=%u records_applied=%u campaign=%s sent=%u received=%u auto_loads=%u phase=%d player=%llu",
         originalSave && originalLoad, saves, loads, sidecarsWritten, sidecarsRead, lastSave.c_str(), lastLoad.c_str(),
         recordsWritten, recordsApplied, campaign.c_str(), filesSent, filesReceived, autoLoads, static_cast<int>(barrier.phase), steam::PlayerId());
    log(line);
}
bool Switch(char const* action, LogFn log)
{
    if (!std::strcmp(action, "resendguardoff")) { resendGuard = false; log("SWITCH resendguardoff ok=1"); return true; }
    if (!std::strcmp(action, "lastownherooff")) { log("SWITCH lastownherooff retired=1"); return true; }
    return false;
}
}
