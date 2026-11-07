// SPDX-License-Identifier: GPL-3.0-or-later
#include "persist.h"

#include "ai.h"
#include "config.h"
#include "containers.h"
#include "crashlog.h"
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
#include "npclease_runtime.h"
#include "remote.h"
#include "saving_native.h"
#include "saving_policy.h"
#include "steam.h"
#include "trade.h"
#include "world.h"

#include <Game.h>
#include <Script.h>
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
using GuiMessageFn = bool(__thiscall*)(gCGUIManager*, GEUInt, GEUInt, GEInt);
using NewSlotFn = bool(__stdcall*)(void*);
GuiMessageFn originalGuiMessage = nullptr;
NewSlotFn originalNewSlot = nullptr;
unsigned char* uiEntries[5]{};
bool uiCallInstalled = false;
unsigned saveRequestsSeen = 0;
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
unsigned char* nativeEntries[5]{}, *nativeImage = nullptr;
size_t nativeImageSize = 0;
bool loadCallInstalled = false;
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
    bool stored = false;
    bool committing = false;
};
Prepared prepared;
struct SaveReply {
    net::SaveNote note{};
    uint32_t binding = 0, nextTry = 0;
    bool pending = false;
};
SaveReply hostReplies[net::MaxPlayers], clientReply;
net::SaveNote incomingNote{};
uint32_t incomingWelcome = 0;
bool receiving = false, incomingFailed = false;
std::vector<uint8_t> incoming[net::FileCount];
uint32_t incomingTotal[net::FileCount]{}, incomingAt = 0;
uint32_t requestAt = 0, requestWelcome = 0;
bool requestPending = false;
std::string requestId;
std::string lastSaveFailure;

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
void DropAutoload(char const* why);
void CancelSave(char const* why);
bool __stdcall SaveDetour(void* self, bCString const* name);
bool __stdcall LoadDetour(void* self, bCString const* name);
bool __fastcall WorldSaveDetour(gCWorld* self, void*, bCString const& name, eCGfxImageData* image, void* metadata, GEU32 size);
bool __fastcall WorldLoadDetour(gCWorld* self, void*, bCString const& name, bool nativeFlag);
bool PupLoadEntry(unsigned char* entry, bool beforeUse = false);
bool ReverifyNative(bool beforeUse = false);
bool NativeReady();
bool __fastcall GuiMessageDetour(gCGUIManager* self, void*, GEUInt message, GEUInt key, GEInt flags);
bool __stdcall MenuNewSlot(void* self);

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

bool Executable(void const* address)
{
    MEMORY_BASIC_INFORMATION info{};
    return address && VirtualQuery(address, &info, sizeof(info)) && info.State == MEM_COMMIT &&
           !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
size_t FindCode(unsigned char* image, size_t size, IMAGE_SECTION_HEADER const* sections,
                unsigned sectionCount, char const* pattern, unsigned& matches)
{
    matches = 0;
    size_t found = saving::native::Missing;
    for (unsigned section = 0; section < sectionCount; ++section) {
        auto const& part = sections[section];
        if (!(part.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const size_t length = part.Misc.VirtualSize;
        if (!saving::native::Range(size, part.VirtualAddress, length) || !Readable(image + part.VirtualAddress, length)) continue;
        unsigned count = 0;
        const size_t at = saving::native::FindUnique(image + part.VirtualAddress, length, pattern, count);
        if (at != saving::native::Missing) found = part.VirtualAddress + at;
        matches += count;
    }
    return matches == 1 ? found : saving::native::Missing;
}
void Unresolved(char const* name, unsigned char* entry, char const* phase)
{
    char bytes[25] = "unreadable";
    if (Readable(entry, 12))
        for (size_t b = 0; b < 12; ++b) Format(bytes + b * 2, 3, "%02x", entry[b]);
    Logf("persist: unresolved %s phase=%s live=%s; paired saves off", name, phase, bytes);
}

bool ResolveNative(unsigned char* (&resolved)[5])
{
    HMODULE game = GetModuleHandleA("Game.dll");
    auto* image = reinterpret_cast<unsigned char*>(game);
    if (!Readable(image, sizeof(IMAGE_DOS_HEADER))) { Log("persist: unresolved Game.dll; paired saves off"); return false; }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096 ||
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
        const size_t skip = i == 3 ? Stolen : 0;
        sites[i] = FindCode(image, size, sections, nt->FileHeader.NumberOfSections, fn.bytes + skip * 2, matches);
        if (sites[i] != saving::native::Missing)
            sites[i] = sites[i] >= skip ? sites[i] - skip : saving::native::Missing;
        const size_t length = saving::native::Decode(fn.bytes).size();
        if (matches == 1 && sites[i] != saving::native::Missing &&
            (!fn.symbol || reinterpret_cast<unsigned char*>(GetProcAddress(game, fn.symbol)) == image + sites[i]) &&
            Readable(image + sites[i], length) && Executable(image + sites[i]) &&
            (saving::native::Matches(image + sites[i], length, fn.bytes) ||
             (i == 3 && PupLoadEntry(image + sites[i])))) {
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
    for (size_t i = 0; i < 5; ++i) {
        auto const& fn = saving::native::UiSites[i];
        unsigned matches = 0;
        const size_t site = FindCode(image, size, sections, nt->FileHeader.NumberOfSections, fn.bytes, matches);
        if (site == saving::native::Missing || (fn.symbol &&
            reinterpret_cast<unsigned char*>(GetProcAddress(game, fn.symbol)) != image + site)) {
            Logf("persist: unresolved %s matches=%u; paired saves off", fn.name, matches);
            return false;
        }
        uiEntries[i] = image + site;
    }
    if (!saving::native::Calls(image, size, uiEntries[2] - image, resolved[4] - image)) {
        Unresolved("Menu.NewSaveSlotCall getter edge", uiEntries[2], "install");
        return false;
    }
    if (!saving::native::Calls(image, size, uiEntries[2] - image + 6, uiEntries[1] - image)) {
        Unresolved("Menu.NewSaveSlotCall allocation edge", uiEntries[2] + 6, "install");
        return false;
    }
    if (!saving::native::Calls(image, size, uiEntries[1] - image + 0x27, resolved[0] - image)) {
        Unresolved("HUD.NewSaveSlot save edge", uiEntries[1] + 0x27, "install");
        return false;
    }
    const bool saveLink = Readable(resolved[0] + 0xb9, 5) && saving::native::Calls(image, size, sites[0] + 0xb9, sites[2]);
    const bool loadLink = Readable(resolved[1] + 0x2b2, 5) && saving::native::Calls(image, size, sites[1] + 0x2b2, sites[3]);
    if (!saveLink) Unresolved("SaveGameFn->WorldSaveGameFn call edge", resolved[0] + 0xb9, "install");
    if (!loadLink) Unresolved("LoadGameFn->WorldLoadGameFn call edge", resolved[1] + 0x2b2, "install");
    if (!saveLink || !loadLink) return false;
    size_t singleton = 0, vtable = 0;
    const uintptr_t table = saving::native::Word(image, sites[4] + 32);
    if (!Readable(reinterpret_cast<void const*>(table), sizeof(void*)) ||
        !saving::native::Links(image, size, reinterpret_cast<uintptr_t>(image), sites, singleton, vtable) ||
        !Readable(image + singleton, 12) || !Readable(image + vtable, sizeof(void*))) {
        Unresolved("FileManagerGetter singleton/vtable layout", resolved[4], "install");
        return false;
    }
    fileManagerGetter = reinterpret_cast<Getter>(resolved[4]);
    fileManagerSingleton = image + singleton;
    fileManagerVtable = image + vtable;
    nativeImage = image;
    nativeImageSize = size;
    std::copy_n(resolved, 5, nativeEntries);
    return true;
}

void* FileManager(bool forLoad)
{
    if (!NativeReady()) return nullptr;
    if (!fileManagerGetter || !Readable(fileManagerSingleton, 12)) {
        Unresolved("FileManagerGetter singleton", static_cast<unsigned char*>(fileManagerSingleton), "before-use");
        return nullptr;
    }
    void* self = fileManagerGetter();
    void* vtable = nullptr;
    void* menu = nullptr;
    if (self != fileManagerSingleton) {
        Logf("persist: native FileManagerGetter returned unexpected singleton load=%d player=%llu", forLoad ? 1 : 0, steam::PlayerId());
        return nullptr;
    }
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
void SaveFailure(char const* code)
{
    lastSaveFailure = code;
    Logf("persist: save refusal reason=%s player=%llu host=%d phase=%u prepared=%d receiving=%d request=%d",
         code, steam::PlayerId(), net::IsHost() ? 1 : 0, static_cast<unsigned>(barrier.phase),
         prepared.valid ? 1 : 0, receiving ? 1 : 0, requestPending ? 1 : 0);
    wchar_t text[500]{};
    _snwprintf_s(text, _TRUNCATE, i18n::Text(i18n::SaveFailureReason), i18n::SaveReason(code));
    if (gCSession::GetInstance().IsGameRunning()) hud::Message(hud::Warning, text);
    else hud::MenuLine(text);
}
void QueueReply(uint8_t to, net::SaveNote const& note)
{
    auto& reply = net::IsHost() ? hostReplies[to] : clientReply;
    reply = {note, net::IsHost() ? net::PeerGeneration(to) : net::WelcomeSerial(), GetTickCount() + 250, true};
    if (net::SendSave(to, note)) reply.pending = false;
    else Logf("persist: checkpoint control retry queued player_slot=%u save=%s op=%u", to, note.save, note.op);
}
void RetryReplies(uint32_t now)
{
    for (uint8_t to = net::IsHost() ? 2 : 1; to < (net::IsHost() ? net::MaxPlayers : 2); ++to) {
        auto& reply = net::IsHost() ? hostReplies[to] : clientReply;
        if (!reply.pending) continue;
        if ((net::IsHost() ? !net::PeerJoined(to) || reply.binding != net::PeerGeneration(to) :
             !net::Joined() || reply.binding != net::WelcomeSerial()) ||
            reply.note.epoch != (net::IsHost() ? epoch : hostWorld.epoch)) { reply = {}; continue; }
        if (static_cast<int32_t>(now - reply.nextTry) < 0) continue;
        reply.nextTry = now + 250;
        if (net::SendSave(to, reply.note)) {
            reply.pending = false;
            Logf("persist: checkpoint control retry sent player_slot=%u save=%s op=%u", to, reply.note.save, reply.note.op);
        }
    }
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

bool KnownPup(HMODULE module)
{
    static HMODULE verified = nullptr;
    if (module == verified && module) return true;
    char path[MAX_PATH]{};
    std::vector<uint8_t> bytes;
    const DWORD length = module ? GetModuleFileNameA(module, path, MAX_PATH) : 0;
    if (!length || length >= MAX_PATH || !ReadAll(path, bytes) || bytes.empty()) {
        Logf("persist: unresolved WorldLoadGameFn PUP identity read path=%s error=%lu; paired saves off", path, GetLastError());
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[32]{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status >= 0) status = BCryptHashData(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0 || !saving::native::Matches(digest, sizeof(digest),
        "60fccba5e27ca544fc1e72fe5ba4cbc33aad107c1cbc90845b275f81b58c349c")) {
        Logf("persist: unresolved WorldLoadGameFn PUP identity path=%s status=%08lx; paired saves off", path, status);
        return false;
    }
    verified = module;
    return true;
}

bool PupRefused(char const* stage, unsigned char* entry, uint32_t bridge, uint32_t receiver, bool beforeUse)
{
    const uintptr_t observed = receiver ? receiver : bridge ? bridge : reinterpret_cast<uintptr_t>(entry);
    auto* at = reinterpret_cast<unsigned char const*>(observed);
    char bytes[19] = "unreadable";
    if (Readable(at, 9))
        for (size_t i = 0; i < 9; ++i) Format(bytes + i * 2, 3, "%02x", at[i]);
    Logf("persist: unresolved WorldLoadGameFn PUP phase=%s stage=%s entry=%p bridge=%08x receiver=%08x observed=%p live=%s; paired saves off",
         beforeUse ? "before-use" : "install", stage, entry, bridge, receiver, at, bytes);
    return false;
}

bool PupLoadEntry(unsigned char* entry, bool beforeUse)
{
    uint32_t bridge = 0, receiver = 0;
    if (!Readable(entry, 6) ||
        !saving::native::PupEntryTarget(entry, 6, reinterpret_cast<uintptr_t>(entry), bridge))
        return PupRefused("entry", entry, bridge, receiver, beforeUse);
    if (!Readable(reinterpret_cast<void const*>(bridge), 9) || !Executable(reinterpret_cast<void const*>(bridge)))
        return PupRefused("shim-memory", entry, bridge, receiver, beforeUse);
    if (!saving::native::PupShimTarget(reinterpret_cast<uint8_t const*>(bridge), 9, bridge, receiver))
        return PupRefused("shim-bytes", entry, bridge, receiver, beforeUse);
    HMODULE pup = nullptr;
    // The callback's actual owner is authoritative, even before the main Script_paru DLL loads.
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(receiver), &pup) || !pup)
        return PupRefused("receiver-module", entry, bridge, receiver, beforeUse);
    auto* image = reinterpret_cast<unsigned char*>(pup);
    if (receiver < reinterpret_cast<uintptr_t>(image) || receiver - reinterpret_cast<uintptr_t>(image) != saving::native::PupReceiverRva)
        return PupRefused("receiver-rva", entry, bridge, receiver, beforeUse);
    if (!KnownPup(pup)) return PupRefused("companion-identity", entry, bridge, receiver, beforeUse);
    if (!Readable(image, sizeof(IMAGE_DOS_HEADER)))
        return PupRefused("companion-memory", entry, bridge, receiver, beforeUse);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096 ||
        !Readable(image + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS32)))
        return PupRefused("companion-header", entry, bridge, receiver, beforeUse);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    auto* sections = IMAGE_FIRST_SECTION(nt);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage != saving::native::PupImageSize ||
        !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96 ||
        !Readable(sections, nt->FileHeader.NumberOfSections * sizeof(*sections)))
        return PupRefused("companion-layout", entry, bridge, receiver, beforeUse);
    unsigned matches = 0;
    if (FindCode(image, saving::native::PupImageSize, sections, nt->FileHeader.NumberOfSections,
                 saving::native::PupReceiverBytes, matches) != saving::native::PupReceiverRva)
        return PupRefused("receiver-pattern", entry, bridge, receiver, beforeUse);
    auto const* code = reinterpret_cast<uint8_t const*>(receiver);
    if (!Readable(code, 41) || !Executable(code) ||
        !saving::native::PupReceiverAt(code, 41, receiver, reinterpret_cast<uintptr_t>(image), nt->OptionalHeader.SizeOfImage))
        return PupRefused("receiver-operands", entry, bridge, receiver, beforeUse);
    // Mutable imports/adapter/semaphore are readiness, not the identity of an installed PUP hook.
    static unsigned char* reportedEntry = nullptr;
    static uint32_t reportedBridge = 0, reportedReceiver = 0;
    static uint32_t readyBridge = 0, readyReceiver = 0;
    if (!beforeUse) {
        if (reportedEntry != entry || reportedBridge != bridge || reportedReceiver != receiver) {
            reportedEntry = entry;
            reportedBridge = bridge;
            reportedReceiver = receiver;
            Logf("persist: WorldLoadGameFn PUP chain entry=%p bridge=%08x receiver=%08x owner=%p; runtime verification pending first use",
                 entry, bridge, receiver, image);
        }
        return true;
    }
    if (!Readable(image + 0x677a0, 8) || !Readable(image + 0x97578, 4) || !Readable(image + 0x99510, 4))
        return PupRefused("runtime-memory", entry, bridge, receiver, beforeUse);
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    if (saving::native::Word(image, 0x677a0) != reinterpret_cast<uintptr_t>(GetProcAddress(kernel, "WaitForSingleObject")) ||
        saving::native::Word(image, 0x677a4) != reinterpret_cast<uintptr_t>(GetProcAddress(kernel, "ReleaseSemaphore")))
        return PupRefused("runtime-imports", entry, bridge, receiver, beforeUse);
    const uintptr_t original = saving::native::Word(image, 0x97578);
    if (!original || original == reinterpret_cast<uintptr_t>(entry) || original == bridge || original == receiver ||
        !Executable(reinterpret_cast<void const*>(original)))
        return PupRefused("runtime-adapter", entry, bridge, receiver, beforeUse);
    DWORD flags = 0;
    if (!GetHandleInformation(reinterpret_cast<HANDLE>(saving::native::Word(image, 0x99510)), &flags))
        return PupRefused("runtime-semaphore", entry, bridge, receiver, beforeUse);
    if (readyBridge != bridge || readyReceiver != receiver) {
        readyBridge = bridge;
        readyReceiver = receiver;
        Logf("persist: WorldLoadGameFn PUP ready entry=%p bridge=%08x receiver=%08x owner=%p adapter=%p",
             entry, bridge, receiver, image, reinterpret_cast<void*>(original));
    }
    return true;
}

bool OwnedHook(unsigned char* entry, void* original, void* detour, size_t stolen, char const* pattern)
{
    uint32_t guard = 0, target = 0;
    const size_t length = saving::native::Decode(pattern).size();
    return Readable(entry, length) &&
        saving::native::PupEntryTarget(entry, stolen, reinterpret_cast<uintptr_t>(entry), guard) &&
        guard == reinterpret_cast<uintptr_t>(original) - 24 &&
        Readable(reinterpret_cast<void const*>(guard), 24 + stolen + 5) &&
        Executable(reinterpret_cast<void const*>(guard)) &&
        saving::native::Matches(reinterpret_cast<uint8_t const*>(guard), 24,
            "9c833d????????0074069de9????????9de9????????") &&
        saving::native::BranchTarget(reinterpret_cast<uint8_t const*>(guard + 11), 5, guard + 11, 0xe9, target) &&
        target == reinterpret_cast<uintptr_t>(original) &&
        saving::native::BranchTarget(reinterpret_cast<uint8_t const*>(guard + 17), 5, guard + 17, 0xe9, target) &&
        target == reinterpret_cast<uintptr_t>(detour) &&
        saving::native::BranchTarget(static_cast<uint8_t const*>(original) + stolen, 5,
            reinterpret_cast<uintptr_t>(original) + stolen, 0xe9, target) &&
        target == reinterpret_cast<uintptr_t>(entry + stolen) &&
        (stolen == Stolen || entry[6] == 0x90) &&
        saving::native::Matches(static_cast<uint8_t const*>(original), stolen,
            stolen == Stolen ? "558bec83e4f8" : "8b44240483ec68") &&
        saving::native::Matches(entry + stolen, length - stolen, pattern + stolen * 2);
}

bool ReverifyNative(bool beforeUse)
{
    if (reinterpret_cast<unsigned char*>(GetModuleHandleA("Game.dll")) != nativeImage) {
        Unresolved("Game.dll", nativeImage, "before-use");
        return false;
    }
    void* originals[3]{reinterpret_cast<void*>(originalSave), reinterpret_cast<void*>(originalLoad),
                       reinterpret_cast<void*>(originalWorldSave)};
    void* detours[3]{reinterpret_cast<void*>(&SaveDetour), reinterpret_cast<void*>(&LoadDetour),
                    reinterpret_cast<void*>(&WorldSaveDetour)};
    for (size_t i = 0; i < 5; ++i) {
        auto const& fn = saving::native::Sites[i];
        auto* entry = nativeEntries[i];
        const size_t length = saving::native::Decode(fn.bytes).size();
        bool ok = Readable(entry, length);
        if (ok && i < 3) {
            ok = OwnedHook(entry, originals[i], detours[i], Stolen, fn.bytes);
        } else if (ok && i == 3) {
            ok = saving::native::Matches(entry, length, fn.bytes) ||
                 (saving::native::Matches(entry + Stolen, length - Stolen, fn.bytes + Stolen * 2) && PupLoadEntry(entry, beforeUse));
        } else if (ok) ok = saving::native::Matches(entry, length, fn.bytes);
        if (ok && fn.symbol)
            ok = reinterpret_cast<unsigned char*>(GetProcAddress(reinterpret_cast<HMODULE>(nativeImage), fn.symbol)) == entry;
        if (!ok) { Unresolved(fn.name, entry, "before-use"); return false; }
    }
    uint32_t target = 0;
    for (size_t i = 0; i < 5; ++i) {
        auto const& fn = saving::native::UiSites[i];
        const size_t length = saving::native::Decode(fn.bytes).size();
        const bool ok = i == 0 ? OwnedHook(uiEntries[0], reinterpret_cast<void*>(originalGuiMessage),
            reinterpret_cast<void*>(&GuiMessageDetour), saving::native::GuiStolen, fn.bytes) :
            Readable(uiEntries[i], length) && saving::native::Matches(uiEntries[i], length, fn.bytes);
        if (!ok || (fn.symbol && reinterpret_cast<unsigned char*>(GetProcAddress(
            reinterpret_cast<HMODULE>(nativeImage), fn.symbol)) != uiEntries[i])) {
            Unresolved(fn.name, uiEntries[i], "before-use");
            return false;
        }
    }
    if (!uiCallInstalled || !saving::native::Calls(nativeImage, nativeImageSize,
        uiEntries[2] - nativeImage, nativeEntries[4] - nativeImage) ||
        !saving::native::Calls(nativeImage, nativeImageSize, uiEntries[1] - nativeImage + 0x27, nativeEntries[0] - nativeImage) ||
        !saving::native::BranchTarget(uiEntries[2] + 6, 5, reinterpret_cast<uintptr_t>(uiEntries[2] + 6), 0xe8, target) ||
        target != reinterpret_cast<uintptr_t>(&MenuNewSlot)) {
        Unresolved("Menu.NewSaveSlotCall edge", uiEntries[2] + 6, "before-use");
        return false;
    }
    auto* call = nativeEntries[1] + 0x2b2;
    if (!Readable(call, 5) || !loadCallInstalled ||
        !saving::native::BranchTarget(call, 5, reinterpret_cast<uintptr_t>(call), 0xe8, target) ||
        target != reinterpret_cast<uintptr_t>(&WorldLoadDetour)) {
        Unresolved("LoadGameFn->WorldLoadGameFn call edge", call, "before-use");
        return false;
    }
    call = nativeEntries[0] + 0xb9;
    if (!Readable(call, 5) ||
        !saving::native::BranchTarget(nativeEntries[0] + 0xb9, 5, reinterpret_cast<uintptr_t>(nativeEntries[0] + 0xb9), 0xe8, target) ||
        target != reinterpret_cast<uintptr_t>(nativeEntries[2])) {
        Unresolved("SaveGameFn->WorldSaveGameFn call edge", call, "before-use");
        return false;
    }
    size_t singleton = 0, table = 0;
    const uintptr_t vtable = saving::native::Word(nativeEntries[4], 32);
    if (!Readable(reinterpret_cast<void const*>(vtable), 4) ||
        !saving::native::GetterLinks(nativeImage, nativeImageSize, reinterpret_cast<uintptr_t>(nativeImage),
                                    nativeEntries[4] - nativeImage, singleton, table) ||
        nativeImage + singleton != fileManagerSingleton || nativeImage + table != fileManagerVtable) {
        Unresolved("FileManagerGetter layout", nativeEntries[4], "before-use");
        return false;
    }
    static unsigned char* reportedLoad = nullptr;
    static bool reportedPup = false;
    const bool pup = nativeEntries[3][0] == 0xe9;
    if (beforeUse && (reportedLoad != nativeEntries[3] || reportedPup != pup)) {
        reportedLoad = nativeEntries[3];
        reportedPup = pup;
        Logf("persist: native reverified before use WorldLoadGameFn=%p owner=%s; original entry preserved",
             nativeEntries[3], pup ? "PUP/paru_s2.dll" : "Game.dll");
    }
    return true;
}

bool NativeReady()
{
    if (!pairedEnabled || ai::ShuttingDown()) return false;
    if (ReverifyNative(true)) return true;
    pairedEnabled = false;
    CancelSave("native-hooks");
    DropAutoload("native hooks changed before use");
    prepared = Prepared{};
    clientReply = {};
    requestPending = false;
    receiving = false;
    incomingFailed = true;
    for (auto& file : incoming) file.clear();
    Log("persist: native pre-use verification failed; paired saves off");
    return false;
}

bool HookNativeCall(unsigned char* call, void* entry, void* detour, char const* label)
{
    uint32_t target = 0;
    if (!Readable(call, 5) ||
        !saving::native::BranchTarget(call, 5, reinterpret_cast<uintptr_t>(call), 0xe8, target) ||
        target != reinterpret_cast<uintptr_t>(entry)) {
        Unresolved(label, call, "install");
        return false;
    }
    unsigned char before[5], after[5];
    std::memcpy(before, call, sizeof(before));
    DWORD old = 0;
    if (!VirtualProtect(call, sizeof(before), PAGE_EXECUTE_READWRITE, &old)) {
        Logf("persist: %s hook failed error=%lu; paired saves off", label, GetLastError());
        return false;
    }
    const uint32_t relative = reinterpret_cast<uintptr_t>(detour) - reinterpret_cast<uintptr_t>(call + 5);
    std::memcpy(call + 1, &relative, sizeof(relative));
    std::memcpy(after, call, sizeof(after));
    DWORD ignored = 0;
    const bool restored = VirtualProtect(call, sizeof(before), old, &ignored) != 0;
    if (!restored) Logf("persist: %s protection restore failed error=%lu; paired saves off", label, GetLastError());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), call, sizeof(before)) != 0;
    if (!flushed) Logf("persist: %s instruction-cache flush failed error=%lu; paired saves off", label, GetLastError());
    ai::TrackPatch(call, before, after, sizeof(before), label);
    return restored && flushed;
}
bool HookWorldLoadCall(unsigned char* call, unsigned char* entry)
{
    return HookNativeCall(call, entry, reinterpret_cast<void*>(&WorldLoadDetour), "paired world-load call");
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
char const* NativeSaveReason()
{
    if (!eCApplication::IsInitialised()) return "not-running";
    if (nativeLoadDepth || nativeSaveDepth || !loading.empty()) return "loading";
    if (npclease::ActiveLeases()) {
        Log("save refusal reason=npc-lease-active");
        hud::Message(hud::Warning, L"NPC lease active: save would lose client-owned NPC state");
        return "npc-lease-active";
    }
    gCSession& session = gCSession::GetInstance();
    if (!session.IsGameRunning() || !session.GetHero()) return "not-running";
    if (session.GetPlayer() != session.GetHero()) return "control";
    if (downed::Held() || downed::NativeDown()) return "downed";
    gCWorld* world = gCWorld::GetCurrentWorld();
    if (!world || !world->GetWorldEntity()) return "world";
    gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
    if (!infos) return "world";
    if (Folder().empty()) return "folder";
    // IsSaveAllowed/StopControlNPC in the verified CP image share this controlled-NPC slot.
    void* controlled = nullptr;
    std::memcpy(&controlled, reinterpret_cast<unsigned char const*>(&session) + 0x28, sizeof(controlled));
    Entity self(session.GetHero());
    char const* reason = saving::NativeSaveRefusal(infos->GetIsRunning() != GEFalse, controlled != nullptr,
                                                  self.IsDead(), self.NPC.DefeatedByPlayer != GEFalse);
    if (reason) return reason;
    return session.IsSaveAllowed() ? nullptr : "native-save";
}
bool NativeSaveAllowed()
{
    if (npclease::ActiveLeases()) {
        Log("save refusal reason=npc-lease-active");
        hud::Message(hud::Warning, L"NPC lease active: save would lose client-owned NPC state");
        return false;
    }
    return NativeSaveReason() == nullptr;
}
char const* SaveReadiness()
{
    if (npclease::ActiveLeases()) {
        Log("save refusal reason=npc-lease-active");
        hud::Message(hud::Warning, L"NPC lease active: save would lose client-owned NPC state");
        return "npc-lease-active";
    }
    if (char const* reason = NativeSaveReason()) return reason;
    if (!ownCharacterReady || !pendingRecord.empty() || !refusedRecord.empty() || !waitingImport.empty()) return "own-character";
    if (!trade::ReadyForSave()) return "trade";
    return nullptr;
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

void CancelClientSave(char const* reason)
{
    if (net::IsHost() || (!prepared.valid && !requestPending)) return;
    net::SaveNote note = prepared.valid ? prepared.note : net::SaveNote{};
    if (!prepared.valid) {
        note.epoch = hostWorld.epoch;
        strncpy_s(note.world, current.key.world.c_str(), _TRUNCATE);
        strncpy_s(note.save, requestId.c_str(), _TRUNCATE);
    }
    note.op = net::SaveCancel;
    note.ok = 0;
    strncpy_s(note.name, reason, _TRUNCATE);
    QueueReply(1, note);
    prepared = {};
    requestPending = false;
    ClearIncoming();
    SaveFailure(reason);
}

bool SendCampaign(uint8_t to, bool commit = false)
{
    if (net::FileTransferBusy(to)) return false;
    Manifest manifest;
    std::vector<uint8_t> data[net::FileCount];
    if (!pairedEnabled || !transferring || !ReadBundle(campaign, manifest, data)) {
        Logf("persist: checkpoint %s is incomplete; not sent to player %u", campaign.c_str(), to);
        if (commit && pairedEnabled && transferring) CancelSave("transfer-manifest");
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
    Logf("persist: checkpoint queued world=%s save=%s native_owner=%llu player_slot=%u mode=%s size=%u/%u/%u crc=%08x/%08x/%08x",
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
            SaveFailure("character-write");
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
    incomingAt = GetTickCount();
}

void OnFileDone(char const* name, uint32_t const size[net::FileCount], uint32_t const crc[net::FileCount])
{
    if (!receiving || incomingWelcome != net::WelcomeSerial() || incomingNote.epoch != hostWorld.epoch) { ClearIncoming(); return; }
    char const* reason = incomingFailed ? "transfer-chunk" :
        !SafeName(name) || name != saving::Slot(incomingNote.save) ? "name" : nullptr;
    for (int f = 0; !reason && f < net::FileCount; ++f) {
        if (!size[f] || incomingTotal[f] != size[f] || incoming[f].size() != size[f]) reason = "transfer-length";
        else if (Crc32(incoming[f].data(), incoming[f].size()) != crc[f]) reason = "transfer-crc";
    }
    Manifest manifest;
    if (!reason && (!ParseManifest(incoming[2], manifest) || !Verify(manifest, incoming) ||
        manifest.key.world != incomingNote.world || manifest.key.save != incomingNote.save)) reason = "transfer-manifest";
    const bool commit = incomingNote.op == net::SaveCommit;
    if (!reason && commit) {
        saving::Key key{incomingNote.world, incomingNote.save, steam::PlayerId()}, found;
        if (!prepared.valid || prepared.welcome != net::WelcomeSerial() ||
            std::strcmp(prepared.note.save, incomingNote.save) || !hero::ReadRecordKey(CharacterPath(key).c_str(), found) ||
            !saving::Exact(key, found)) reason = "character-record";
    }
    if (!reason && !StoreBundle(name, incoming)) reason = "disk-write";
    if (!reason && commit) {
        const char* alias = prepared.note.name;
        if (!SafeName(alias) || saving::Prefix(alias, "G3T_") || !StoreBundle(alias, incoming)) reason = "alias-write";
        if (reason) Logf("persist: client checkpoint alias not published save=%s slot=%s player=%llu",
                      incomingNote.save, alias, steam::PlayerId());
    }
    if (reason) {
        Logf("persist: checkpoint transfer refused save=%s player=%llu mode=%s size=%u/%u/%u crc=%08x/%08x/%08x reason=%s",
             incomingNote.save, steam::PlayerId(), commit ? "store" : "load",
             size[0], size[1], size[2], crc[0], crc[1], crc[2], reason);
        SaveFailure(reason);
        if (commit) {
            net::SaveNote answer = incomingNote;
            answer.op = net::SaveStored;
            answer.ok = 0;
            strncpy_s(answer.name, reason, _TRUNCATE);
            QueueReply(1, answer);
            prepared = {};
            requestPending = false;
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
        net::SaveNote answer = Note(net::SaveStored, manifest, latestOk);
        if (!latestOk) strncpy_s(answer.name, "character-write", _TRUNCATE);
        QueueReply(1, answer);
        prepared.stored = latestOk;
        if (!latestOk) { prepared = {}; SaveFailure("character-write"); }
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
    if (!ok) net::CancelFileTransfers(savingManifest.key.save.c_str());
    Logf("persist: checkpoint %s world=%s save=%s player=%llu slot=%s reason=%s",
         ok ? "paired" : "incomplete", savingManifest.key.world.c_str(), savingManifest.key.save.c_str(),
         steam::PlayerId(), requestedSlot.c_str(), why);
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        if (!barrier.members[i].generation || !net::PeerJoined(i) || net::PeerGeneration(i) != barrier.members[i].generation) continue;
        net::SaveNote result = Note(ok ? net::SaveResult : net::SaveCancel, savingManifest, ok);
        if (!ok) strncpy_s(result.name, why, _TRUNCATE);
        QueueReply(i, result);
    }
    if (ok) Notice(i18n::SavePaired);
    else SaveFailure(why);
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
    const bool leaseOk = !net::IsHost() || npclease::WriteSnapshot(file);
    bool ok = leaseOk && !ferror(file);
    ok = fflush(file) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (ok) ok = ReadAll(path, savedJournal);
    DeleteFileA(path.c_str());
    return ok;
}

void CALLBACK SaveNow(HWND, UINT, UINT_PTR, DWORD)
{
    if (processDepth || nativeLoadDepth || nativeSaveDepth || barrier.phase != saving::Barrier::Preparing || !barrier.AllReady()) return;
    if (npclease::ActiveLeases()) {
        CancelSave("NPC lease active: save would lose client-owned NPC state");
        return;
    }
    if (barrier.epoch != epoch) { CancelSave("world"); return; }
    if (char const* reason = SaveReadiness()) { CancelSave(reason); return; }
    const std::string slot = saving::Slot(barrier.save);
    void* manager = FileManager(false);
    if (!manager || !originalSave || !writing) { CancelSave("native-hooks"); return; }
    if (!CaptureJournal(slot)) { CancelSave("journal-write"); return; }
    if (!CaptureCharacter(savingManifest.key)) { CancelSave("character-write"); return; }
    ++recordsWritten;
    if (requestedSlot == "QuickSave") {
        // The verified native capture helper takes its file manager in ESI, not ECX.
        void* capture = uiEntries[4];
        __asm {
            push esi
            mov esi, manager
            call capture
            pop esi
        }
    }
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
    if (!ok) { CancelSave("native-save"); return; }
    nativeReturnedAt = GetTickCount();
    barrier.phase = saving::Barrier::Writing;
    Logf("persist: native checkpoint save returned world=%s save=%s player=%llu; waiting for disk completion",
         savingManifest.key.world.c_str(), savingManifest.key.save.c_str(), savingManifest.key.player);
}

bool StartSave(char const* name)
{
    if (!pairedEnabled || !NativeReady() || !writing) { SaveFailure("native-hooks"); return false; }
    if (!SafeName(name) || saving::Prefix(name, "G3T_")) { SaveFailure("name"); return false; }
    if (barrier.phase != saving::Barrier::Idle) { SaveFailure("busy"); return false; }
    if (char const* reason = SaveReadiness()) { SaveFailure(reason); return false; }
    savingManifest = Manifest{};
    savingManifest.key = {current.key.world.empty() ? RandomId() : current.key.world, RandomId(), steam::PlayerId()};
    if (!saving::Valid(savingManifest.key)) {
        Logf("persist: checkpoint identity unavailable world=%s save=%s player=%llu; paired saves off for this save",
             savingManifest.key.world.c_str(), savingManifest.key.save.c_str(), steam::PlayerId());
        SaveFailure("identity");
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
        if (!net::InWorld(i)) { CancelSave("peer-loading"); return false; }
        barrier.members[i].generation = net::PeerGeneration(i);
        net::SaveNote prepare = Note(net::SavePrepare, savingManifest);
        strncpy_s(prepare.name, requestedSlot.c_str(), _TRUNCATE);
        if (!net::SendSave(i, prepare)) { CancelSave("control-send"); return false; }
    }
    saveWindow = eCApplication::GetInstance().GetHandle();
    if (!saveWindow || !SetTimer(saveWindow, SaveTimer, 50, &SaveNow)) { CancelSave("timer"); return false; }
    Logf("persist: checkpoint requested world=%s save=%s player=%llu slot=%s epoch=%08x",
         savingManifest.key.world.c_str(), savingManifest.key.save.c_str(), steam::PlayerId(), name, epoch);
    Notice(i18n::SaveQueued);
    return true;
}

bool __stdcall SaveDetour(void* self, bCString const* name)
{
    if (!pairedEnabled) return originalSave && originalSave(self, name);
    ++saveRequestsSeen;
    if (!NativeReady()) { SaveFailure("native-hooks"); return false; }
    if (!name || !SafeName(name->GetText())) {
        Log("persist: save refused; the name is not a SafeName");
        SaveFailure("name");
        return false;
    }
    if (char const* reason = SaveReadiness()) { SaveFailure(reason); return false; }
    if (net::IsHost()) StartSave(name->GetText());
    else if (!net::Joined()) SaveFailure("host-offline");
    else if (!net::InWorld(net::LocalId())) SaveFailure("peer-loading");
    else if (requestPending || prepared.valid) SaveFailure("busy");
    else {
        net::SaveNote note{};
        note.op = net::SaveRequest;
        note.epoch = hostWorld.epoch;
        requestId = RandomId();
        if (requestId.empty()) { SaveFailure("identity"); return false; }
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
        } else SaveFailure("control-send");
    }
    // The engine has not saved yet. Do not return success for a queued distributed operation.
    return false;
}

bool __fastcall GuiMessageDetour(gCGUIManager* self, void*, GEUInt message, GEUInt key, GEInt flags)
{
    if (ai::ShuttingDown() || !pairedEnabled || message != 0x8001)
        return originalGuiMessage(self, message, key, flags);
    if (!NativeReady()) { SaveFailure("native-hooks"); return true; }
    const bCString slot("QuickSave");
    SaveDetour(nullptr, &slot);
    return true; // UI event handled, not native save success.
}

bool __stdcall MenuNewSlot(void* self)
{
    if (ai::ShuttingDown() || !pairedEnabled) return originalNewSlot(self);
    if (!NativeReady()) { SaveFailure("native-hooks"); return true; }
    const unsigned before = saveRequestsSeen;
    originalNewSlot(self); // Retain the game's menu-slot allocation; SaveDetour queues the request.
    if (before == saveRequestsSeen) SaveFailure("slot-allocation");
    return true; // Dismiss the request UI; only SaveResult may announce durable success.
}

bool __fastcall WorldSaveDetour(gCWorld* self, void*, bCString const& name, eCGfxImageData* image, void* metadata, GEU32 size)
{
    // HUD/manual/quicksave reaches this only under our native-save scope. Other native callers join the same protocol.
    if (!pairedEnabled) return originalWorldSave(self, name, image, metadata, size);
    if (!NativeReady()) { SaveFailure("native-hooks"); return false; }
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
    if (ai::ShuttingDown() || !pairedEnabled) return originalWorldLoad(self, name, nativeFlag);
    if (!NativeReady()) { SaveFailure("native-hooks"); return false; }
    const bool observed = nativeLoadDepth != 0;
    if (pairedEnabled && !observed) {
        Logf("persist: load refused function=World.LoadGameWorld slot=%s player=%llu; paired saves off for this load, use HUD.LoadGame",
             name.GetText(), steam::PlayerId());
        SaveFailure("native-load");
        return false;
    }
    if (observed) ++lowerLoadCalls;
    if (observed) Logf("persist: native invoke=World.LoadGameWorld save=%s player=%llu flag=%d",
                       current.key.save.c_str(), steam::PlayerId(), nativeFlag ? 1 : 0);
    crashlog::WorldEntryScope operation("native World.LoadGameWorld / PUP bridge");
    const bool ok = originalWorldLoad(self, name, nativeFlag);
    operation.Complete();
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
    if (barrier.epoch != epoch) { CancelSave("world"); return; }
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        if (!barrier.members[i].generation) continue;
        if (!net::PeerJoined(i) || net::PeerGeneration(i) != barrier.members[i].generation) {
            CancelSave("peer-left");
            return;
        }
    }
    const uint32_t now = GetTickCount();
    if ((barrier.phase == saving::Barrier::Preparing && now - saveStarted > PrepareLimit) ||
        (barrier.phase == saving::Barrier::Writing && now - nativeReturnedAt > WriteLimit) ||
        (barrier.phase == saving::Barrier::Committing && now - saveStarted > TransferLimit)) {
        CancelSave(barrier.phase == saving::Barrier::Preparing ? "prepare-timeout" :
                   barrier.phase == saving::Barrier::Writing ? "write-timeout" : "transfer-timeout");
        return;
    }
    if (barrier.phase == saving::Barrier::Preparing) return; // native call is exclusively in SaveNow
    if (barrier.phase == saving::Barrier::Committing) {
        for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
            auto& member = barrier.members[i];
            if (!member.generation || member.queued) continue;
            const bool queued = SendCampaign(i, true);
            if (barrier.phase != saving::Barrier::Committing) return;
            member.queued = queued;
        }
        if (barrier.AllStored()) FinishSave(aliasPublished, aliasPublished ? "all participant stores acknowledged" : "alias-write");
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
    if (!WriteAtomic(Path(slot, 2), data[2])) { CancelSave("disk-write"); return; }
    ++sidecarsWritten;
    if (!PublishLatest(savingManifest.key)) { CancelSave("character-write"); return; }
    campaign = slot;
    current = savingManifest;
    localKind = net::KindSaved;
    localSave = slot;
    lastSave = requestedSlot;
    aliasPublished = StoreBundle(requestedSlot, data);
    if (!aliasPublished) { CancelSave("alias-write"); return; }
    barrier.phase = saving::Barrier::Committing;
    saveStarted = now;
    Logf("persist: host checkpoint published world=%s save=%s player=%llu alias=%s alias_ok=%d size=%u/%u/%zu crc=%08x/%08x/%08x",
         current.key.world.c_str(), current.key.save.c_str(), current.key.player, requestedSlot.c_str(), aliasPublished ? 1 : 0,
         current.size[0], current.size[1], data[2].size(), current.crc[0], current.crc[1], Crc32(data[2].data(), data[2].size()));
}

void OnSave(uint8_t from, net::SaveNote const& note)
{
    if (!pairedEnabled || !NativeReady()) {
        if (note.op == net::SaveRequest || note.op == net::SavePrepare) {
            net::SaveNote answer = note;
            answer.op = net::IsHost() ? net::SaveResult : net::SaveReady;
            answer.ok = 0;
            strncpy_s(answer.name, "native-hooks", _TRUNCATE);
            QueueReply(net::IsHost() ? from : 1, answer);
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
                const char* reason = !net::InWorld(from) ? "peer-loading" : !SafeName(note.name) ? "name" : lastSaveFailure.c_str();
                strncpy_s(answer.name, reason, _TRUNCATE);
                QueueReply(from, answer);
            }
        } else if ((note.op == net::SaveReady || note.op == net::SaveStored || note.op == net::SaveCancel) &&
                   barrier.Matches(note.save, note.epoch) &&
                   (note.world == savingManifest.key.world || (note.op == net::SaveCancel && !*note.world))) {
            if (barrier.members[from].generation != net::PeerGeneration(from) ||
                !barrier.members[from].generation ||
                (note.op == net::SaveReady && barrier.phase != saving::Barrier::Preparing) ||
                (note.op == net::SaveStored && barrier.phase != saving::Barrier::Committing)) return;
            if (!note.ok) {
                Logf("persist: checkpoint partner refused player_slot=%u save=%s op=%u reason=%s", from, note.save, note.op, note.name);
                CancelSave(note.name);
                return;
            }
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
            if (prepared.stored) answer.op = net::SaveStored;
            QueueReply(1, answer);
            return;
        }
        if (!saving::Valid(key) || !SafeName(note.name) || saving::Prefix(note.name, "G3T_")) {
            net::SaveNote answer = note;
            answer.op = net::SaveReady;
            answer.ok = 0;
            strncpy_s(answer.name, "name", _TRUNCATE);
            QueueReply(1, answer);
            SaveFailure("name");
            return;
        }
        if (prepared.valid) {
            Logf("persist: prepared checkpoint superseded previous=%s save=%s player=%llu", prepared.note.save, note.save, steam::PlayerId());
            prepared = {};
            ClearIncoming();
        }
        const char* reason = !net::InWorld(net::LocalId()) ? "peer-loading" : SaveReadiness();
        if (!reason && !CaptureCharacter(key)) reason = "character-write";
        const bool ok = !reason;
        if (ok) {
            prepared = {note, net::WelcomeSerial(), GetTickCount(), true};
            ++recordsWritten;
            requestPending = false;
        }
        net::SaveNote answer = note;
        answer.op = net::SaveReady;
        answer.ok = ok ? 1 : 0;
        if (reason) strncpy_s(answer.name, reason, _TRUNCATE);
        QueueReply(1, answer);
        requestPending = false;
        Logf("persist: checkpoint character prepare world=%s save=%s player=%llu ok=%d", note.world, note.save, steam::PlayerId(), ok ? 1 : 0);
        if (!ok) SaveFailure(reason);
    } else if (note.op == net::SaveCommit || note.op == net::SaveImport) {
        if (!SafeName(note.name) || note.name != saving::Slot(note.save)) return;
        if (note.op == net::SaveCommit && (!prepared.valid || prepared.welcome != net::WelcomeSerial() ||
            std::strcmp(prepared.note.save, note.save) || std::strcmp(prepared.note.world, note.world))) return;
        ClearIncoming();
        incomingNote = note;
        incomingWelcome = net::WelcomeSerial();
        receiving = true;
        incomingAt = GetTickCount();
        if (note.op == net::SaveCommit) { prepared.since = incomingAt; prepared.committing = true; }
        Logf("persist: checkpoint receive begin save=%s player=%llu mode=%s epoch=%08x",
             note.save, steam::PlayerId(), note.op == net::SaveCommit ? "store" : "load", note.epoch);
    } else if (note.op == net::SaveCancel || note.op == net::SaveResult) {
        const bool relevant = prepared.valid ? !std::strcmp(prepared.note.save, note.save) :
                              (requestPending && requestId == note.save) || current.key.save == note.save ||
                              std::string(clientReply.note.save) == note.save;
        if (!relevant) return;
        prepared = Prepared{};
        requestPending = false;
        if (clientReply.note.save == std::string(note.save)) clientReply = {};
        if (!note.ok && receiving && !std::strcmp(incomingNote.save, note.save)) ClearIncoming();
        Logf("persist: host checkpoint result world=%s save=%s player=%llu ok=%u", note.world, note.save, steam::PlayerId(), note.ok);
        if (note.ok) Notice(i18n::SavePaired);
        else SaveFailure(note.name);
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
    if (net::IsHost()) return;
    if (!net::Joined()) { crashlog::WorldEntryWait("host welcome"); return; }
    if (!hostWorld.epoch) { crashlog::WorldEntryWait("host world announcement"); return; }
    if (nativeLoadDepth) { crashlog::WorldEntryWait("native HUD.LoadGame return"); return; }
    if (!loading.empty() || newWorld) { crashlog::WorldEntryWait("engine hero/game readiness"); return; }
    if (!pendingRecord.empty() || !refusedRecord.empty() || !waitingImport.empty() || !ownCharacterReady) {
        crashlog::WorldEntryWait(hostWorld.kind == net::KindMenu ? "host settled world" :
                                !pendingRecord.empty() ? "own character restore" :
                                !refusedRecord.empty() ? "validated own character" :
                                !waitingImport.empty() ? "own character import" : "own character readiness");
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
        crashlog::WorldEntryWait(nullptr);
        return;
    }
    crashlog::WorldEntryWait(hostWorld.kind == net::KindMenu ? "host settled world" : "matching host world");
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
    CancelSave("world");
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
        CancelClientSave("world");
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
        if (note.epoch != hostWorld.epoch || note.kind != hostWorld.kind ||
            std::strcmp(note.campaign, hostWorld.campaign))
            Logf("persist: host world received previous=%08x epoch=%08x kind=%s local=%s loading=%s new_world=%d own=%d",
                 hostWorld.epoch, note.epoch, KindName(note.kind), KindName(localKind),
                 loading.empty() ? "-" : loading.c_str(), newWorld ? 1 : 0, ownCharacterReady ? 1 : 0);
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
        const bool onWay = resendGuard && client.sentAt && !client.arrived && GetTickCount() - client.sentAt < TransferLimit;
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
    if (processDepth || nativeLoadDepth || nativeSaveDepth) {
        crashlog::WorldEntryWait(nativeLoadDepth ? "native load before autoload" :
                                nativeSaveDepth ? "native save before autoload" : "engine frame before autoload");
        return;
    }
    if (net::IsHost() || !net::Joined() || autoload.welcome != net::WelcomeSerial() ||
        autoload.epoch != hostWorld.epoch || autoload.loads != loads) { DropAutoload("world/welcome/load changed"); return; }
    if (autoloadHold) { crashlog::WorldEntryWait("autoload hold"); return; }
    const std::string slot = autoload.slot;
    KillTimer(window, id);
    autoload = Autoload{};
    void* manager = FileManager(true);
    if (!manager || !originalLoad) {
        Logf("persist: autoload preflight failed save=%s player=%llu; paired load not invoked", slot.c_str(), steam::PlayerId());
        SaveFailure("native-load");
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
    if (!NativeReady()) { SaveFailure("native-hooks"); return false; }
    if (!name || !SafeName(name->GetText()) || nativeLoadDepth || nativeSaveDepth || !trade::ReadyForSave()) {
        Log("persist: load refused (unsafe name, nested native operation or unsettled exchange)");
        SaveFailure(!name || !SafeName(name->GetText()) ? "name" :
                    nativeLoadDepth || nativeSaveDepth ? "loading" : "trade");
        return false;
    }
    CancelSave("canceled-load");
    CancelClientSave("canceled-load");
    if (net::IsHost()) net::CancelFileTransfers();
    if (self != FileManager(true)) {
        Logf("persist: native load preflight failed player=%llu; paired load not invoked", steam::PlayerId());
        SaveFailure("native-load");
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
        if (!AdoptLegacy(slot, manifest)) { Logf("persist: legacy adoption failed slot=%s", slot.c_str()); SaveFailure("disk-write"); return false; }
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
                SaveFailure("disk-write");
                return false;
            }
            Logf("persist: canonical bundle repaired from verified alias slot=%s canonical=%s", slot.c_str(), canonical.c_str());
        }
    }
    pendingRecord.clear();
    refusedRecord.clear();
    const bool overlay = managed && (manifest.legacy.empty() || !net::IsHost() || manifest.key.player != steam::PlayerId());
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
    npclease::SaveOverlay leaseOverlay;
    bool leaseRead = false;
    if (!fopen_s(&journalFile, Path(slot, 2).c_str(), "r") && journalFile) {
        char header[32]{};
        if (fgets(header, sizeof(header), journalFile) && (!std::strncmp(header, "G3T 1", 5) || !std::strncmp(header, "G3T 2", 5))) {
            journal::Read(journalFile);
            if (net::IsHost()) {
                fseek(journalFile, 0, SEEK_SET);
                npcs::ReadCompanions(journalFile);
                rewind(journalFile);
                leaseRead = npclease::ReadSnapshot(journalFile, leaseOverlay);
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
    crashlog::WorldEntryWait("native HUD.LoadGame return");
    crashlog::WorldEntryScope operation("native HUD.LoadGame");
    const bool hudOk = originalLoad(self, name);
    operation.Complete();
    --nativeLoadDepth;
    crashlog::WorldEntryWait("engine hero/game readiness");
    const bool ok = hudOk && lowerLoadCalls == 1 && lowerLoadGood;
    if (ok && net::IsHost()) {
        if (leaseRead) npclease::Restore(leaseOverlay);
        else npclease::RejectSnapshot("missing or invalid");
    }
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
        SaveFailure("native-load");
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
    crashlog::WorldEntryScope operation("own character validation/restoration");
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
        crashlog::WorldEntryWait(nativeLoadDepth ? "native HUD.LoadGame return" : "native save return");
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
    RetryReplies(now);
    StatusHints();
    gCSession& game = gCSession::GetInstance();
    if (game.GetHero() && game.IsGameRunning()) {
        if (!loading.empty()) {
            const std::string slot = loading;
            loading.clear();
            newWorld = false;
            Settled(net::KindSaved, slot);
        } else if (newWorld) { newWorld = false; Settled(net::KindNew, ""); }
    } else if (newWorld || !loading.empty()) {
        crashlog::WorldEntryWait("engine hero/game readiness");
    }
    if (net::IsHost()) { PollSave(); HostTick(); }
    else {
        if (receiving && (!net::Joined() || incomingWelcome != net::WelcomeSerial() ||
            incomingNote.epoch != hostWorld.epoch || now - incomingAt > TransferLimit)) {
            Logf("persist: checkpoint receive expired save=%s player=%llu mode=%s",
                 incomingNote.save, steam::PlayerId(), incomingNote.op == net::SaveCommit ? "store" : "load");
            ClearIncoming();
            if (incomingNote.op == net::SaveCommit) {
                net::SaveNote answer = incomingNote;
                answer.op = net::SaveStored;
                answer.ok = 0;
                strncpy_s(answer.name, "transfer-timeout", _TRUNCATE);
                QueueReply(1, answer);
                prepared = {};
            }
            requestPending = false;
            SaveFailure(!net::Joined() ? "peer-left" : incomingNote.epoch != hostWorld.epoch ? "world" : "transfer-timeout");
        }
        if (!waitingImport.empty() && (!net::Joined() || waitingEpoch != hostWorld.epoch || waitingWelcome != net::WelcomeSerial())) {
            Log("persist: deferred host import canceled after world/welcome changed");
            waitingImport.clear();
        }
        if (prepared.valid && (!net::Joined() || prepared.welcome != net::WelcomeSerial() ||
            prepared.note.epoch != hostWorld.epoch ||
            now - prepared.since > (prepared.committing ? TransferLimit : PreparedLimit))) {
            Logf("persist: prepared checkpoint expired save=%s player=%llu", prepared.note.save, steam::PlayerId());
            const char* reason = !net::Joined() ? "peer-left" :
                prepared.note.epoch != hostWorld.epoch ? "world" : "transfer-timeout";
            CancelClientSave(reason);
        }
        if (requestPending && (!net::Joined() || requestWelcome != net::WelcomeSerial() || now - requestAt > PrepareLimit)) {
            requestPending = false;
            SaveFailure(!net::Joined() ? "host-offline" : "request-timeout");
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
    // Keep PUP's export entry and generated calling-convention bridge untouched.
    originalWorldLoad = reinterpret_cast<WorldLoadFn>(resolved[3]);
    loadCallInstalled = HookWorldLoadCall(resolved[1] + 0x2b2, resolved[3]);
    originalGuiMessage = reinterpret_cast<GuiMessageFn>(ai::JumpTo(uiEntries[0],
        reinterpret_cast<void*>(&GuiMessageDetour), saving::native::GuiStolen));
    originalNewSlot = reinterpret_cast<NewSlotFn>(uiEntries[1]);
    uiCallInstalled = HookNativeCall(uiEntries[2] + 6, uiEntries[1], reinterpret_cast<void*>(&MenuNewSlot), "paired menu-save call");
    pairedEnabled = originalSave && originalLoad && originalWorldSave && originalWorldLoad && loadCallInstalled &&
                    originalGuiMessage && uiCallInstalled;
    if (pairedEnabled) pairedEnabled = ReverifyNative();
    if (originalSave) saveTarget = resolved[0];
    if (originalLoad) loadTarget = resolved[1];
    installed = true;
    if (!pairedEnabled) {
        Logf("persist: save/load hook installation failed SaveGameFn=%d LoadGameFn=%d WorldSaveGameFn=%d WorldLoadGameFn=%d; partial hooks pass through, paired saves off",
             originalSave != nullptr, originalLoad != nullptr, originalWorldSave != nullptr, loadCallInstalled ? 1 : 0);
        return;
    }
    Logf("persist: save and load hooked SaveGameFn=%p LoadGameFn=%p WorldSaveGameFn=%p WorldLoadGameFn=%p FileManagerGetter=%p protocol=%u",
         resolved[0], resolved[1], resolved[2], resolved[3], resolved[4], saving::Protocol);
    Logf("persist: paired save UI hooked GUI.HandleMessage=%p Menu.NewSaveSlotCall=%p Session.IsSaveAllowed=%p HUD.CaptureSaveImage=%p",
         uiEntries[0], uiEntries[2] + 6, uiEntries[3], uiEntries[4]);
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
