// SPDX-License-Identifier: GPL-3.0-or-later
#include "persist.h"

#include "ai.h"
#include "config.h"
#include "containers.h"
#include "crashlog.h"
#include "dialogaudio.h"
#include "downed.h"
#include "doors.h"
#include "hero.h"
#include "hud.h"
#include "i18n.h"
#include "journal.h"
#include "log.h"
#include "lobby.h"
#include "net.h"
#include "npcs.h"
#include "npclease_runtime.h"
#include "npclease_save.h"
#include "remote.h"
#include "saving_native.h"
#include "saving_policy.h"
#include "saving_store.h"
#include "steam.h"
#include "story.h"
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
constexpr UINT_PTR SaveTimer = 0x47335453;
using FileFn = bool(__stdcall*)(void*, bCString const*);
using Getter = void*(__cdecl*)();
using WorldSaveFn = bool(__thiscall*)(gCWorld*, bCString const&, eCGfxImageData*, void*, GEU32);
using WorldLoadFn = bool(__thiscall*)(gCWorld*, bCString const&, bool);
using GuiMessageFn = bool(__thiscall*)(gCGUIManager*, GEUInt, GEUInt, GEInt);
using NewSlotFn = bool(__stdcall*)(void*);
using DeleteFn = bool(__thiscall*)(gCWorld*, bCString const&);
DeleteFn originalDelete = nullptr;
unsigned char* deleteTarget = nullptr;
unsigned lowerSaveCalls = 0;
bool lowerSaveGood = true;
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
bool installed = false, pairedEnabled = false, writing = true;
unsigned processDepth = 0, nativeLoadDepth = 0, nativeSaveDepth = 0;
unsigned saves = 0, loads = 0;
unsigned recordsWritten = 0, recordsApplied = 0, loadFrames = 0, epochs = 0;
std::string lastSave, lastLoad, campaign, loading;
uint8_t localKind = net::KindMenu;
bool newWorld = false, ownCharacterReady = false, needsNewHero = false, restoreFailed = false;
bool freshLocalHero = false, resetLocalWorld = false;
bool hostAdmissionAck = false;
uint32_t nextAdmissionTry = 0;
uint32_t seenWelcome = 0;
uint32_t epoch = 0, synced = 0;
net::WorldNote hostWorld{};
using Bytes = saving::store::Bytes;
using Save = saving::store::Save;
Save current, savingManifest;
std::string requestedSlot, stagingSlot, lastSaveFailure;
bool nativeSaved = false;
uint32_t nativeSavedAt = 0;
saving::store::IoError lastIoFailure;
struct PendingCleanup { std::string path; uint32_t until = 0, nextTry = 0; };
std::vector<PendingCleanup> pendingCleanup;
saving::Key pendingKey;
HWND saveWindow = nullptr;

saving::CaptureRound operation;
struct Client {
    uint32_t generation = 0, ready = 0, recordSent = 0, nextTry = 0, cutover = 0;
};
Client clients[net::MaxPlayers]{};
struct Captured {
    net::SaveNote note{};
    Bytes bytes;
    uint32_t welcome = 0, started = 0, nextTry = 0;
    bool valid = false;
    bool queued = false;
};
Captured captured;
net::SaveNote requested{};
uint32_t requestWelcome = 0, requestStarted = 0, requestRetry = 0;
bool requestPending = false;
struct Reply {
    net::SaveNote note{};
    uint32_t generation = 0, nextTry = 0, expires = 0;
    bool pending = false;
};
Reply hostReplies[net::MaxPlayers], clientReply;
struct RequestCache {
    std::string token, transaction;
    net::SaveNote result{};
    uint32_t generation = 0, epoch = 0;
    bool complete = false;
};
RequestCache requests[net::MaxPlayers]{};
struct ExtensionEntry {
    std::string name;
    uint32_t type = 0, version = 0;
    SaveExtensions::Capture capture = nullptr;
    SaveExtensions::Restore restore = nullptr;
};
std::vector<ExtensionEntry> extensions;
uint32_t restoreAt = 0, receivedWelcome = 0;
Bytes receivedRecord, receivedPersonal;
net::CharacterNote receivedNote{};

void Evaluate();
void CancelSave(char const* why);
bool __stdcall SaveDetour(void* self, bCString const* name);
bool __stdcall LoadDetour(void* self, bCString const* name);
bool __fastcall WorldSaveDetour(gCWorld*, void*, bCString const&, eCGfxImageData*, void*, GEU32);
bool __fastcall WorldLoadDetour(gCWorld*, void*, bCString const&, bool);
bool PupLoadEntry(unsigned char*, bool beforeUse = false);
bool ReverifyNative(bool beforeUse = false);
bool NativeReady();
bool __fastcall GuiMessageDetour(gCGUIManager*, void*, GEUInt, GEUInt, GEInt);
bool __stdcall MenuNewSlot(void*);
bool __fastcall DeleteDetour(gCWorld*, void*, bCString const&);
bool ReadAll(std::string const&, Bytes&);
void SaveFailure(char const*);
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
    Logf("persist: unresolved %s phase=%s live=%s; host-managed saves off", name, phase, bytes);
}

bool ResolveNative(unsigned char* (&resolved)[5])
{
    HMODULE game = GetModuleHandleA("Game.dll");
    auto* image = reinterpret_cast<unsigned char*>(game);
    if (!Readable(image, sizeof(IMAGE_DOS_HEADER))) { Log("persist: unresolved Game.dll; host-managed saves off"); return false; }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096 ||
        !Readable(image + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS32))) {
        Log("persist: unresolved Game.dll PE header; host-managed saves off");
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || !size || size > 128u * 1024 * 1024 ||
        !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96 ||
        !Readable(sections, nt->FileHeader.NumberOfSections * sizeof(*sections))) {
        Log("persist: unresolved Game.dll PE layout; host-managed saves off");
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
        Logf("persist: unresolved %s matches=%u hint=%08lx live=%s match=%p export=%p; host-managed saves off",
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
            Logf("persist: unresolved %s matches=%u; host-managed saves off", fn.name, matches);
            return false;
        }
        uiEntries[i] = image + site;
    }
    auto const& deletion = saving::native::DeleteSite;
    unsigned deleteMatches = 0;
    const size_t deleteRva = FindCode(image, size, sections, nt->FileHeader.NumberOfSections, deletion.bytes, deleteMatches);
    if (deleteRva == saving::native::Missing || reinterpret_cast<unsigned char*>(GetProcAddress(game, deletion.symbol)) != image + deleteRva) {
        Logf("persist: unresolved %s matches=%u", deletion.name, deleteMatches);
        return false;
    }
    deleteTarget = image + deleteRva;
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

void LogIo(saving::store::IoError const& error)
{
    Logf("persist: file operation=%s path=%s destination=%s error=%lu",
         error.operation, error.path.c_str(), error.destination.c_str(), static_cast<unsigned long>(error.code));
}
bool ReadAll(std::string const& path, std::vector<uint8_t>& out)
{
    if (saving::store::Read(path, out, &lastIoFailure)) return true;
    LogIo(lastIoFailure);
    return false;
}

bool KnownPup(HMODULE module)
{
    static HMODULE verified = nullptr;
    if (module == verified && module) return true;
    char path[MAX_PATH]{};
    std::vector<uint8_t> bytes;
    const DWORD length = module ? GetModuleFileNameA(module, path, MAX_PATH) : 0;
    if (!length || length >= MAX_PATH || !ReadAll(path, bytes) || bytes.empty()) {
        Logf("persist: unresolved WorldLoadGameFn PUP identity read path=%s error=%lu; host-managed saves off", path, GetLastError());
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
        Logf("persist: unresolved WorldLoadGameFn PUP identity path=%s status=%08lx; host-managed saves off", path, status);
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
    Logf("persist: unresolved WorldLoadGameFn PUP phase=%s stage=%s entry=%p bridge=%08x receiver=%08x observed=%p live=%s; host-managed saves off",
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

bool OwnedHook(unsigned char* entry, void* original, void* detour, size_t stolen, char const* pattern, char const* prefix = nullptr)
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
            prefix ? prefix : stolen == Stolen ? "558bec83e4f8" : "8b44240483ec68") &&
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
    if (!OwnedHook(deleteTarget, reinterpret_cast<void*>(originalDelete), reinterpret_cast<void*>(&DeleteDetour),
                   Stolen, saving::native::DeleteSite.bytes, "83ec10535657")) {
        Unresolved(saving::native::DeleteSite.name, deleteTarget, "before-use");
        return false;
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
    ownCharacterReady = false;
    Log("persist: native pre-use verification failed; host-managed saves disabled");
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
        Logf("persist: %s hook failed error=%lu; host-managed saves off", label, GetLastError());
        return false;
    }
    const uint32_t relative = reinterpret_cast<uintptr_t>(detour) - reinterpret_cast<uintptr_t>(call + 5);
    std::memcpy(call + 1, &relative, sizeof(relative));
    std::memcpy(after, call, sizeof(after));
    DWORD ignored = 0;
    const bool restored = VirtualProtect(call, sizeof(before), old, &ignored) != 0;
    if (!restored) Logf("persist: %s protection restore failed error=%lu; host-managed saves off", label, GetLastError());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), call, sizeof(before)) != 0;
    if (!flushed) Logf("persist: %s instruction-cache flush failed error=%lu; host-managed saves off", label, GetLastError());
    ai::TrackPatch(call, before, after, sizeof(before), label);
    return restored && flushed;
}
bool HookWorldLoadCall(unsigned char* call, unsigned char* entry)
{
    return HookNativeCall(call, entry, reinterpret_cast<void*>(&WorldLoadDetour), "host world-load call");
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
char const* SaveReadiness()
{
    if (npclease::ActiveLeases()) {
        Log("save refusal reason=npc-lease-active");
        hud::Message(hud::Warning, L"NPC lease active: save would lose client-owned NPC state");
        return "npc-lease-active";
    }
    if (char const* reason = NativeSaveReason()) return reason;
    if (!ownCharacterReady) return "own-character";
    if (!trade::ReadyForSave()) return "trade";
    return nullptr;
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
    if (!net::IsHost() && !net::Joined()) { SaveFailure("host-offline"); return true; }
    if (!net::IsHost() && !net::InWorld(net::LocalId())) { SaveFailure("peer-loading"); return true; }
    if (char const* reason = SaveReadiness()) { SaveFailure(reason); return true; }
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
        ++lowerSaveCalls;
        Logf("persist: native invoke=World.SaveGameWorld save=%s player=%llu metadata_size=%lu",
             savingManifest.key.save.c_str(), steam::PlayerId(), size);
        const bool ok = originalWorldSave(self, name, image, metadata, size);
        lowerSaveGood = lowerSaveGood && ok;
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
        Logf("persist: load refused function=World.LoadGameWorld slot=%s player=%llu; host-managed saves off for this load, use HUD.LoadGame",
             name.GetText(), steam::PlayerId());
        SaveFailure("native-load");
        return false;
    }
    if (observed) ++lowerLoadCalls;
    if (observed) Logf("persist: native invoke=World.LoadGameWorld save=%s player=%llu flag=%d",
                       current.key.save.c_str(), steam::PlayerId(), nativeFlag ? 1 : 0);
    crashlog::WorldEntryScope scope("native World.LoadGameWorld / PUP bridge");
    const bool ok = originalWorldLoad(self, name, nativeFlag);
    scope.Complete();
    if (observed) lowerLoadGood = lowerLoadGood && ok;
    if (observed) Logf("persist: native result=World.LoadGameWorld save=%s player=%llu ok=%d",
                       current.key.save.c_str(), steam::PlayerId(), ok ? 1 : 0);
    return ok;
}

void Notice(i18n::Message message, bool bad = false)
{
    if (gCSession::GetInstance().IsGameRunning()) hud::Message(bad ? hud::Warning : hud::Info, i18n::Text(message));
    else hud::MenuLine(i18n::Text(message));
}
void SaveFailure(char const* code)
{
    lastSaveFailure = code;
    Logf("persist: save refusal reason=%s player=%llu host=%d active=%d", code, steam::PlayerId(),
         net::IsHost() ? 1 : 0, operation.active ? 1 : 0);
    wchar_t text[500]{};
    _snwprintf_s(text, _TRUNCATE, i18n::Text(i18n::SaveFailureReason), i18n::SaveReason(code));
    if (gCSession::GetInstance().IsGameRunning()) hud::Message(hud::Warning, text);
    else hud::MenuLine(text);
}
bool WriteAtomic(std::string const& path, Bytes const& bytes)
{
    if (saving::store::Atomic(path, bytes, nullptr, &lastIoFailure)) {
        Logf("persist: file operation=atomic-publish path=%s bytes=%zu ok=1", path.c_str(), bytes.size());
        return true;
    }
    LogIo(lastIoFailure);
    return false;
}
std::string CharacterPath()
{
    const std::string folder = Folder();
    return folder.empty() ? std::string() : folder + "g3t-character-" + std::to_string(steam::PlayerId()) + "-local.txt";
}
bool ValidateCharacter(Bytes const& bytes, saving::Key const& key)
{
    saving::Key found;
    if (!saving::store::HeroKey(bytes, found) || !saving::Exact(found, key)) return false;
    const std::string base = CharacterPath();
    if (base.empty()) return false;
    const std::string path = base + ".validate";
    if (!WriteAtomic(path, bytes)) return false;
    const bool ok = hero::ValidateRecord(path.c_str(), key);
    DeleteFileA(path.c_str());
    return ok;
}
bool CaptureCharacter(saving::Key const& key, Bytes& bytes)
{
    const std::string path = CharacterPath();
    const bool ok = !path.empty() && hero::SaveRecord(path.c_str(), key) && ReadAll(path, bytes) &&
                    bytes.size() <= saving::store::CharacterLimit && ValidateCharacter(bytes, key);
    Logf("persist: character capture world=%s save=%s player=%llu ok=%d bytes=%zu crc=%08x",
         key.world.c_str(), key.save.c_str(), key.player, ok ? 1 : 0, bytes.size(), saving::store::Crc(bytes));
    if (ok) ++recordsWritten;
    return ok;
}
bool CapturePlayer(saving::Key const& key, Bytes& bytes, char const*& reason)
{
    Bytes hero, state;
    if (!CaptureCharacter(key, hero)) { reason = "character-write"; return false; }
    if (!containers::CapturePersonal(state) || !saving::store::PackPlayer(hero, state, bytes)) {
        reason = "personal-record"; return false;
    }
    Logf("persist: player envelope captured account=%llu hero_bytes=%zu personal_bytes=%zu total=%zu",
         key.player, hero.size(), state.size(), bytes.size());
    return true;
}
bool PersonalReadyForRestore(Bytes const& bytes)
{
    if (bytes.empty() || containers::PersonalHooksReady()) return true;
    saving::personal::Snapshot snapshot;
    if (!saving::personal::Decode(bytes, snapshot)) return false;
    for (auto const& entry : snapshot)
        if (static_cast<uint8_t>(entry.second.kind) != 4) return false;
    return true;
}
net::SaveNote Note(uint8_t op, Save const& save, bool ok = true)
{
    net::SaveNote note{};
    note.op = op; note.ok = ok ? 1 : 0; note.epoch = net::IsHost() ? epoch : hostWorld.epoch;
    strncpy_s(note.world, save.key.world.c_str(), _TRUNCATE);
    strncpy_s(note.save, save.key.save.c_str(), _TRUNCATE);
    strncpy_s(note.name, requestedSlot.c_str(), _TRUNCATE);
    return note;
}
void QueueReply(uint8_t to, net::SaveNote const& note)
{
    if (to >= net::MaxPlayers) return;
    auto& reply = net::IsHost() ? hostReplies[to] : clientReply;
    const uint32_t now = GetTickCount();
    reply = {note, net::IsHost() ? net::PeerGeneration(to) : net::WelcomeSerial(), now + 250, now + 10000, true};
    if (net::SendSave(to, note)) reply.pending = false;
}
void RetryReplies(uint32_t now)
{
    for (uint8_t to = net::IsHost() ? 2 : 1; to < (net::IsHost() ? net::MaxPlayers : 2); ++to) {
        auto& reply = net::IsHost() ? hostReplies[to] : clientReply;
        if (!reply.pending) continue;
        if (static_cast<int32_t>(now - reply.expires) >= 0 ||
            (net::IsHost() ? !net::PeerJoined(to) || reply.generation != net::PeerGeneration(to)
                          : !net::Joined() || reply.generation != net::WelcomeSerial()) ||
            reply.note.epoch != (net::IsHost() ? epoch : hostWorld.epoch)) { reply.pending = false; continue; }
        if (static_cast<int32_t>(now - reply.nextTry) < 0) continue;
        reply.nextTry = now + 250;
        if (net::SendSave(to, reply.note)) reply.pending = false;
    }
}
bool SendRecord(uint8_t to, uint8_t purpose, saving::Key const& key, Bytes const& bytes)
{
    net::CharacterNote note{};
    note.purpose = purpose; note.epoch = net::IsHost() ? epoch : hostWorld.epoch;
    note.account = key.player; note.length = static_cast<uint32_t>(bytes.size()); note.crc = saving::store::Crc(bytes);
    strncpy_s(note.world, key.world.c_str(), _TRUNCATE); strncpy_s(note.save, key.save.c_str(), _TRUNCATE);
    const bool ok = net::SendCharacter(to, note, bytes);
    Logf("persist: character send purpose=%u peer=%u account=%llu save=%s bytes=%zu crc=%08x queued=%d",
         purpose, to, key.player, note.save, bytes.size(), note.crc, ok ? 1 : 0);
    return ok;
}
void StopSaveTimer()
{
    if (saveWindow) KillTimer(saveWindow, SaveTimer);
    saveWindow = nullptr;
}
void CleanupStaging()
{
    if (stagingSlot.empty()) return;
    for (int f = 0; f < 2; ++f) {
        const std::string path = Path(stagingSlot, f);
        for (auto const& item : {path, path + ".tmp"}) {
            saving::store::IoError error;
            const bool removed = saving::store::Remove(item, &error);
            if (!removed) {
                LogIo(error);
            }
            // Retain a short tombstone for a native file that appears after failure cleanup.
            if (removed || saving::store::Busy(error.code))
                pendingCleanup.push_back({item, GetTickCount() + 3000, GetTickCount() + 200});
        }
    }
    stagingSlot.clear();
}
void RetryCleanup(uint32_t now)
{
    for (auto it = pendingCleanup.begin(); it != pendingCleanup.end();) {
        if (static_cast<int32_t>(now - it->nextTry) < 0) { ++it; continue; }
        saving::store::IoError error;
        const bool removed = saving::store::Remove(it->path, &error);
        if (static_cast<int32_t>(now - it->until) >= 0 || (!removed && !saving::store::Busy(error.code))) {
            if (!removed) LogIo(error);
            it = pendingCleanup.erase(it);
        } else { it->nextTry = now + 200; ++it; }
    }
}
void FinishSave(bool ok, char const* why)
{
    if (!operation.active) return;
    Logf("persist: host save %s world=%s save=%s slot=%s elapsed_ms=%u reason=%s",
         ok ? "complete" : "failed", savingManifest.key.world.c_str(), savingManifest.key.save.c_str(),
         requestedSlot.c_str(), GetTickCount() - operation.started, why);
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        if (!operation.members[i].generation || operation.members[i].generation != net::PeerGeneration(i)) continue;
        auto result = Note(net::SaveResult, savingManifest, ok);
        if (!ok) strncpy_s(result.name, why, _TRUNCATE);
        auto& request = requests[i];
        if (request.transaction == savingManifest.key.save && request.generation == net::PeerGeneration(i)) {
            request.result = result;
            request.complete = true;
        }
        QueueReply(i, result);
    }
    StopSaveTimer();
    operation = {};
    nativeSaved = false;
    CleanupStaging();
    if (ok) Notice(i18n::SaveComplete); else SaveFailure(why);
}
void CancelSave(char const* why) { FinishSave(false, why); }
bool AllReady()
{
    return operation.AllReady();
}
bool CaptureJournal(Bytes& bytes)
{
    const std::string path = Path(stagingSlot, 2) + ".state";
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") || !file) return false;
    journal::Write(file); npcs::WriteCompanions(file);
    fprintf(file, "downed %d\n", downed::Held() ? 1 : 0);
    bool ok = !ferror(file);
    ok = fflush(file) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (ok) ok = ReadAll(path, bytes);
    DeleteFileA(path.c_str());
    return ok;
}
bool CaptureExtensions(Save& save)
{
    for (auto const& entry : extensions) {
        Bytes bytes;
        if (!entry.capture(bytes) || bytes.size() > saving::store::ExtensionLimit) {
            Logf("persist: extension capture refused name=%s type=%u bytes=%zu", entry.name.c_str(), entry.type, bytes.size());
            return false;
        }
        saving::store::Set(save, {entry.type, entry.version, 0, std::move(bytes)});
    }
    return true;
}
bool RestoreExtensions()
{
    const Bytes empty;
    for (auto const& entry : extensions) {
        auto* block = saving::store::Find(current, entry.type);
        if (!entry.restore(block ? block->version : 0, block ? block->bytes : empty)) {
            Logf("persist: extension restore refused name=%s type=%u version=%u",
                 entry.name.c_str(), entry.type, block ? block->version : 0);
            return false;
        }
    }
    return true;
}
constexpr uint32_t NpcLeaseExtension = saving::store::Extension;
bool CaptureNpcLease(Bytes& bytes)
{
    std::string text;
    if (!npclease::EncodeSaveText(npclease::Capture(), text)) return false;
    bytes.assign(text.begin(), text.end());
    return bytes.size() <= saving::store::ExtensionLimit;
}
bool RestoreNpcLease(uint32_t savedVersion, Bytes const& bytes)
{
    if (!savedVersion) {
        npclease::OnWorldDeactivate();
        return true;
    }
    if (savedVersion != 1 || bytes.size() > npclease::MaxOverlayText) return false;
    std::string text(bytes.begin(), bytes.end());
    npclease::SaveOverlay overlay;
    if (!npclease::DecodeSaveText(text, overlay)) return false;
    return npclease::Restore(overlay, epoch);
}
bool Materialize(std::string const& slot, Save const& save)
{
    const std::string paths[2] = {Path(slot, 0), Path(slot, 1)};
    saving::store::IoError recovery;
    const bool ok = saving::store::Repair(paths, save, lastIoFailure, recovery);
    if (!ok) LogIo(lastIoFailure);
    if (recovery.code) LogIo(recovery);
    Logf("persist: file operation=cache-repair header=%s world=%s ok=%d",
         paths[0].c_str(), paths[1].c_str(), ok ? 1 : 0);
    return ok;
}
bool TracePublish(saving::store::PublishStage stage, saving::store::IoError&)
{
    const bool marker = stage == saving::store::PublishStage::Marker;
    const int file = stage == saving::store::PublishStage::World ? 1 :
                     stage == saving::store::PublishStage::Header ? 0 : 2;
    Logf("persist: file operation=publish-stage stage=%u path=%s",
         static_cast<unsigned>(stage), (Path(requestedSlot, file) + (marker ? ".publish" : "")).c_str());
    return true;
}
bool __fastcall DeleteDetour(gCWorld* self, void*, bCString const& name)
{
    if (!pairedEnabled || nativeSaveDepth || ai::ShuttingDown()) return originalDelete(self, name);
    if (!SafeName(name.GetText())) { SaveFailure("name"); return false; }
    if (!NativeReady()) { SaveFailure("native-hooks"); return false; }
    const std::string slot = name.GetText(), path = Path(slot, 2);
    if (operation.active && requestedSlot == slot) CancelSave("canceled-delete");
    if (!Exists(path)) return originalDelete(self, name);
    Bytes bytes;
    Save intact;
    const bool recoverable = ReadAll(path, bytes) && saving::store::Decode(bytes, intact);
    const bool nativeOk = originalDelete(self, name);
    const bool gone = !Exists(Path(slot, 0)) && !Exists(Path(slot, 1));
    if (gone && (!Exists(path) || DeleteFileA(path.c_str()))) {
        Logf("persist: host slot deleted slot=%s native_ok=%d", slot.c_str(), nativeOk ? 1 : 0);
        return true;
    }
    const DWORD failure = GetLastError();
    if (recoverable && !Materialize(slot, intact)) Logf("persist: delete recovery failed slot=%s", slot.c_str());
    Logf("persist: slot deletion failed slot=%s native_ok=%d error=%lu; container retained recoverable=%d",
         slot.c_str(), nativeOk ? 1 : 0, failure, recoverable ? 1 : 0);
    SaveFailure(saving::store::IoReason({ "delete", path, {}, failure }, "save-delete-failed"));
    return false;
}
void CALLBACK SaveNow(HWND, UINT, UINT_PTR, DWORD)
{
    if (npclease::ActiveLeases()) {
        CancelSave("npc-lease-active");
        return;
    }
    if (processDepth || nativeLoadDepth || nativeSaveDepth || !operation.active || !AllReady()) return;
    if (operation.epoch != epoch) { CancelSave("world"); return; }
    if (operation.Expired(GetTickCount())) { CancelSave("world-state"); return; }
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        auto const& member = operation.members[i];
        if (member.generation && (!net::InWorld(i) || member.generation != net::PeerGeneration(i) ||
                                  member.account != net::PeerAccount(i))) { CancelSave("peer-left"); return; }
    }
    if (!nativeSaved) {
        if (char const* reason = SaveReadiness()) { CancelSave(reason); return; }
        if (!doors::SettleForSave()) return;
        void* manager = FileManager(false);
        if (!manager || !writing) { CancelSave("native-hooks"); return; }
        Bytes hero, state;
        if (!CaptureCharacter(savingManifest.key, hero)) { CancelSave("character-write"); return; }
        saving::store::Set(savingManifest, {saving::store::Character, 4, steam::PlayerId(), std::move(hero)});
        if (!containers::CapturePersonal(state)) { CancelSave("personal-record"); return; }
        saving::store::Set(savingManifest, {saving::store::Personal, 1, steam::PlayerId(), std::move(state)});
        if (!CaptureJournal(state)) { CancelSave("journal-write"); return; }
        saving::store::Set(savingManifest, {saving::store::Journal, 1, 0, std::move(state)});
        if (!CaptureExtensions(savingManifest)) { CancelSave("extension"); return; }
        {
            void* capture = uiEntries[4];
            __asm {
                push esi
                mov esi, manager
                call capture
                pop esi
            }
        }
        remote::DespawnAll("host-managed save");
        const bCString name(stagingSlot.c_str());
        Logf("persist: native invoke=HUD.SaveGame save=%s slot=%s player=%llu",
             savingManifest.key.save.c_str(), stagingSlot.c_str(), steam::PlayerId());
        lowerSaveCalls = 0; lowerSaveGood = true;
        ++nativeSaveDepth;
        const bool nativeOk = originalSave(manager, &name);
        --nativeSaveDepth;
        ++saves;
        Logf("persist: native result=HUD.SaveGame save=%s ok=%d lower_calls=%u lower_ok=%d",
             savingManifest.key.save.c_str(), nativeOk ? 1 : 0, lowerSaveCalls, lowerSaveGood ? 1 : 0);
        if (!nativeOk || lowerSaveCalls != 1 || !lowerSaveGood) { CancelSave("native-save"); return; }
        nativeSaved = true;
        nativeSavedAt = GetTickCount();
        Logf("persist: native files expected header=%s world=%s",
             Path(stagingSlot, 0).c_str(), Path(stagingSlot, 1).c_str());
    }
    for (int f = 0; f < 2; ++f) {
        const uint32_t type = f ? saving::store::NativeWorld : saving::store::NativeHeader;
        if (saving::store::Find(savingManifest, type)) continue;
        Bytes bytes;
        saving::store::IoError error;
        const std::string path = Path(stagingSlot, f);
        std::string source;
        if (!saving::store::ReadNative(path, bytes, error, source) || bytes.empty()) {
            if (!error.code) error = {"native-empty", source, {}, ERROR_HANDLE_EOF};
            LogIo(error);
            if (saving::store::RetryNative(error, GetTickCount() - nativeSavedAt)) return;
            CancelSave(saving::store::NativeReason(error)); return;
        }
        Logf("persist: file operation=native-read path=%s bytes=%zu native_rename_recovered=%d ok=1",
             source.c_str(), bytes.size(), source != path ? 1 : 0);
        saving::store::Set(savingManifest, {type, 1, 0, std::move(bytes)});
    }
    Bytes encoded;
    if (!saving::store::Encode(savingManifest, encoded)) {
        Logf("persist: container encoding refused save=%s blocks=%zu", savingManifest.key.save.c_str(), savingManifest.blocks.size());
        CancelSave("container-format"); return;
    }
    const std::string paths[3] = {Path(requestedSlot, 0), Path(requestedSlot, 1), Path(requestedSlot, 2)};
    saving::store::IoError error, recovery;
    const bool published = saving::store::Publish(paths, savingManifest, encoded, error, recovery, &TracePublish);
    if (recovery.code) LogIo(recovery);
    if (!published) {
        LogIo(error);
        const bool container = error.path == paths[2] + ".tmp" || error.path == paths[2];
        const char* reason = !container ? "cache-publish-failed" :
                             !std::strcmp(error.operation, "rename") ? "publish-rename-failed" : "container-write-failed";
        CancelSave(saving::store::IoReason(error, reason)); return;
    }
    current = savingManifest;
    lastSave = requestedSlot;
    // Saving is not a world change: do not reset epochs, admission, or the client's live character.
    Logf("persist: host container published save=%s slot=%s bytes=%zu records=%zu menu_ok=%d network_world_bytes=0",
         current.key.save.c_str(), requestedSlot.c_str(), encoded.size(), current.blocks.size(), 1);
    FinishSave(true, "durable host container");
}
bool StartSave(char const* name)
{
    if (!pairedEnabled || !NativeReady() || !writing) { SaveFailure("native-hooks"); return false; }
    if (!SafeName(name) || saving::Prefix(name, "G3T_pending_")) { SaveFailure("name"); return false; }
    if (operation.active) {
        if (requestedSlot == name) return true;
        SaveFailure("busy"); return false;
    }
    if (char const* reason = SaveReadiness()) { SaveFailure(reason); return false; }
    savingManifest = {};
    savingManifest.key = {saving::Id(current.key.world) ? current.key.world : RandomId(), RandomId(), steam::PlayerId()};
    if (!saving::Valid(savingManifest.key)) { SaveFailure("identity"); return false; }
    // Retain absent accounts and unknown extensions, but never data from another campaign.
    for (auto block : current.blocks) {
        if (block.type == saving::store::Character && block.account != steam::PlayerId()) {
            if (!saving::store::Rebind(block.bytes, {savingManifest.key.world, savingManifest.key.save, block.account})) {
                SaveFailure("character-record"); return false;
            }
            savingManifest.blocks.push_back(std::move(block));
            bool present = false;
            for (uint8_t i = 2; i < net::MaxPlayers; ++i)
                if (net::PeerJoined(i) && net::PeerAccount(i) == savingManifest.blocks.back().account) present = true;
            if (!present) Logf("persist: retained absent character account=%llu save=%s; no offline progress captured",
                               savingManifest.blocks.back().account, savingManifest.key.save.c_str());
        } else if ((block.type == saving::store::Personal && block.account != steam::PlayerId()) ||
                   block.type >= saving::store::Extension) savingManifest.blocks.push_back(std::move(block));
    }
    requestedSlot = name; stagingSlot = "G3T_pending_" + savingManifest.key.save;
    nativeSaved = false;
    operation = {};
    operation.active = true; operation.epoch = epoch; operation.started = GetTickCount();
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        if (!net::PeerJoined(i)) continue;
        const uint64_t account = net::PeerAccount(i);
        if (!net::InWorld(i)) { CancelSave("peer-loading"); return false; }
        if (!account || account == steam::PlayerId()) { CancelSave("identity"); return false; }
        for (uint8_t j = 2; j < i; ++j)
            if (operation.members[j].account == account) { CancelSave("identity"); return false; }
        operation.members[i] = {net::PeerGeneration(i), 0, account, false};
        QueueReply(i, Note(net::SavePrepare, savingManifest));
    }
    saveWindow = eCApplication::GetInstance().GetHandle();
    if (!saveWindow || !SetTimer(saveWindow, SaveTimer, 30, &SaveNow)) { CancelSave("timer"); return false; }
    Logf("persist: host save requested world=%s save=%s slot=%s epoch=%08x", savingManifest.key.world.c_str(),
         savingManifest.key.save.c_str(), name, epoch);
    Notice(i18n::SaveQueued);
    return true;
}
bool __stdcall SaveDetour(void* self, bCString const* name)
{
    if (!pairedEnabled) return originalSave && originalSave(self, name);
    ++saveRequestsSeen;
    if (!NativeReady()) { SaveFailure("native-hooks"); return false; }
    if (!name || !SafeName(name->GetText()) || saving::Prefix(name->GetText(), "G3T_pending_")) { SaveFailure("name"); return false; }
    if (net::IsHost()) { StartSave(name->GetText()); return false; }
    if (!net::Joined()) { SaveFailure("host-offline"); return false; }
    if (!net::InWorld(net::LocalId())) { SaveFailure("peer-loading"); return false; }
    if (requestPending || captured.valid) { SaveFailure("busy"); return false; }
    if (char const* reason = SaveReadiness()) { SaveFailure(reason); return false; }
    requested = {};
    requested.op = net::SaveRequest; requested.ok = 1; requested.epoch = hostWorld.epoch;
    const std::string id = RandomId(), world = saving::Id(current.key.world) ? current.key.world : std::string(32, '0');
    if (id.empty()) { SaveFailure("identity"); return false; }
    strncpy_s(requested.save, id.c_str(), _TRUNCATE); strncpy_s(requested.world, world.c_str(), _TRUNCATE);
    strncpy_s(requested.name, name->GetText(), _TRUNCATE);
    requestPending = true; requestWelcome = net::WelcomeSerial(); requestStarted = GetTickCount(); requestRetry = requestStarted + 500;
    net::SendSave(1, requested); // Retry admission without allocating a different request.
    Logf("persist: client save requested token=%s slot=%s epoch=%08x", requested.save, requested.name, requested.epoch);
    Notice(i18n::SaveQueued);
    return false; // UI wrappers own the asynchronous outcome.
}
void PollSave(uint32_t now)
{
    if (!operation.active) return;
    if (operation.epoch != epoch) { CancelSave("world"); return; }
    if (operation.Expired(now)) { CancelSave(AllReady() ? "world-state" : "prepare-timeout"); return; }
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        auto& member = operation.members[i];
        if (!member.generation) continue;
        if (!net::PeerJoined(i) || member.generation != net::PeerGeneration(i) || member.account != net::PeerAccount(i)) {
            CancelSave("peer-left"); return;
        }
        if (!member.ready && static_cast<int32_t>(now - member.nextTry) >= 0) {
            member.nextTry = now + 500;
            QueueReply(i, Note(net::SavePrepare, savingManifest));
        }
    }
}
void OnSave(uint8_t from, net::SaveNote const& note)
{
    if (!pairedEnabled) return;
    if (net::IsHost()) {
        if (from < 2 || from >= net::MaxPlayers || note.epoch != epoch) return;
        if (note.op == net::SaveRequest) {
            auto& cached = requests[from];
            if (cached.token == note.save && cached.generation == net::PeerGeneration(from) && cached.epoch == epoch) {
                if (cached.complete) QueueReply(from, cached.result);
                else if (operation.active && cached.transaction == savingManifest.key.save)
                    QueueReply(from, Note(net::SavePrepare, savingManifest));
                return;
            }
            cached = {note.save, {}, {}, net::PeerGeneration(from), epoch, false};
            const char* refusal = !net::InWorld(from) ? "peer-loading" : !SafeName(note.name) ? "name" : nullptr;
            if (refusal || !StartSave(note.name)) {
                auto result = note; result.op = net::SaveResult; result.ok = 0;
                strncpy_s(result.name, refusal ? refusal : lastSaveFailure.c_str(), _TRUNCATE);
                cached.result = result; cached.complete = true;
                QueueReply(from, result);
            } else {
                cached.transaction = savingManifest.key.save;
                // Prepare identifies the transaction even when the request raced the host's same-slot save.
                QueueReply(from, Note(net::SavePrepare, savingManifest));
            }
        } else if (note.op == net::SaveCancel && operation.active && !*note.world && !note.ok &&
                   requests[from].token == note.save && requests[from].transaction == savingManifest.key.save &&
                   requests[from].generation == net::PeerGeneration(from)) CancelSave("request-timeout");
        else if ((note.op == net::SaveReady || note.op == net::SaveCancel) && operation.active &&
                   note.save == savingManifest.key.save && note.world == savingManifest.key.world &&
                   operation.members[from].generation == net::PeerGeneration(from) && !note.ok) CancelSave(note.name);
        return;
    }
    if (note.epoch != hostWorld.epoch) return;
    if (note.op == net::SavePrepare) {
        const saving::Key key{note.world, note.save, steam::PlayerId()};
        if (!saving::Valid(key) || !SafeName(note.name)) return;
        if (captured.valid && captured.note.save == std::string(note.save) && captured.welcome == net::WelcomeSerial()) {
            if (!captured.queued) captured.queued = SendRecord(1, net::CharacterCapture, key, captured.bytes);
            return;
        }
        const char* reason = !net::InWorld(net::LocalId()) ? "peer-loading" : SaveReadiness();
        Bytes bytes;
        if (!reason) CapturePlayer(key, bytes, reason);
        requestPending = false;
        if (reason) {
            auto answer = note; answer.op = net::SaveReady; answer.ok = 0;
            strncpy_s(answer.name, reason, _TRUNCATE); QueueReply(1, answer); SaveFailure(reason); return;
        }
        captured = {note, std::move(bytes), net::WelcomeSerial(), GetTickCount(), GetTickCount() + 500, true};
        captured.queued = SendRecord(1, net::CharacterCapture, key, captured.bytes);
    } else if (note.op == net::SaveResult || note.op == net::SaveCancel) {
        if (!((captured.welcome == net::WelcomeSerial() && captured.note.save == std::string(note.save)) ||
              (requestPending && requested.save == std::string(note.save)))) return;
        captured = {}; requestPending = false;
        Logf("persist: host save result save=%s ok=%u reason=%s", note.save, note.ok, note.name);
        if (note.ok) Notice(i18n::SaveComplete); else SaveFailure(note.name);
    }
}
void OnCharacter(uint8_t from, net::CharacterNote const& note, Bytes const& bytes)
{
    const saving::Key key{note.world, note.save, note.account};
    if (net::IsHost()) {
        if (!operation.active || note.epoch != epoch || from < 2 || from >= net::MaxPlayers ||
            key.world != savingManifest.key.world || key.save != savingManifest.key.save) return;
        auto& member = operation.members[from];
        if (!member.generation || member.generation != net::PeerGeneration(from) ||
            member.account != key.player || net::PeerAccount(from) != key.player) return;
        if (member.ready) return;
        Bytes hero, state;
        if (!saving::store::UnpackPlayer(bytes, hero, state) || !ValidateCharacter(hero, key)) {
            CancelSave("character-record"); return;
        }
        saving::store::Set(savingManifest, {saving::store::Character, 4, key.player, std::move(hero)});
        saving::store::Set(savingManifest, {saving::store::Personal, 1, key.player, std::move(state)});
        operation.Accept(from, net::PeerGeneration(from), key.player, note.epoch);
        Logf("persist: character received save=%s account=%llu bytes=%zu all_ready=%d",
             note.save, key.player, bytes.size(), AllReady() ? 1 : 0);
        return;
    }
    if (note.epoch != hostWorld.epoch || key.player != steam::PlayerId()) return;
    if (synced == hostWorld.epoch && ownCharacterReady) {
        // Duplicate restoration after admission must never rewind a live character.
        Evaluate(); return;
    }
    if (receivedNote.save == std::string(note.save) && receivedNote.epoch == note.epoch &&
        receivedWelcome == net::WelcomeSerial() && (!receivedRecord.empty() || needsNewHero)) return;
    receivedNote = note; receivedWelcome = net::WelcomeSerial(); restoreAt = GetTickCount() + 100;
    receivedRecord.clear(); receivedPersonal.clear(); restoreFailed = false; synced = 0;
    ownCharacterReady = false; hostAdmissionAck = false; nextAdmissionTry = 0; net::SetInWorld(net::LocalId(), false);
    current = {}; current.key = key;
    if (bytes.empty()) {
        needsNewHero = true;
        if (!containers::RestorePersonal({})) { restoreFailed = true; SaveFailure("personal-record"); return; }
        if (freshLocalHero && !resetLocalWorld && gCSession::GetInstance().IsGameRunning()) {
            needsNewHero = false; freshLocalHero = false; ownCharacterReady = true;
            synced = hostWorld.epoch; story::PrepareAdmission(); doors::PrepareAdmission(); Evaluate();
        } else Notice(i18n::SaveCharacterMissing, true);
        Logf("persist: no saved character account=%llu epoch=%08x; fresh New Game required", key.player, note.epoch);
    } else {
        needsNewHero = false;
        saving::Key embedded;
        if (!saving::store::UnpackPlayer(bytes, receivedRecord, receivedPersonal) ||
            !saving::store::HeroKey(receivedRecord, embedded) || !saving::Exact(embedded, key)) {
            restoreFailed = true; receivedRecord.clear(); SaveFailure("character-record");
        }
    }
}
void NewEpoch()
{
    CancelSave("world");
    static const uint32_t nonce = ((GetTickCount() * 2654435761u) ^ (GetCurrentProcessId() << 16)) & 0xffff0000u;
    epoch = nonce | (++epochs & 0xffff);
    if (!epoch) epoch = ++epochs;
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        clients[i].ready = clients[i].recordSent = clients[i].cutover = 0;
        net::SetInWorld(i, false);
        if (!net::PeerJoined(i)) continue;
        net::WorldNote notice{};
        notice.op = net::WorldHost; notice.kind = net::KindMenu; notice.epoch = epoch;
        strncpy_s(notice.campaign, campaign.c_str(), _TRUNCATE);
        net::SendWorld(notice, i);
    }
    Logf("persist: host world epoch=%08x kind=%u campaign=%s", epoch, localKind, campaign.c_str());
}
void Evaluate()
{
    if (net::IsHost() || !net::Joined() || !hostWorld.epoch || hostWorld.kind == net::KindMenu) return;
    if (seenWelcome != net::WelcomeSerial()) { net::SetInWorld(net::LocalId(), false); return; }
    const bool ready = !nativeLoadDepth && loading.empty() && !newWorld && ownCharacterReady &&
                       !needsNewHero && !restoreFailed && synced == hostWorld.epoch &&
                       story::AdmissionReady() && doors::AdmissionReady();
    bool admitted = ready && hostAdmissionAck && net::InWorld(net::LocalId());
    net::WorldNote answer{};
    answer.op = ready ? net::WorldReady : net::WorldWaiting; answer.kind = localKind; answer.epoch = hostWorld.epoch;
    if (ready && hostAdmissionAck) answer.flags = net::WorldEntryConfirmed;
    strncpy_s(answer.campaign, hostWorld.campaign, _TRUNCATE);
    const uint32_t now = GetTickCount();
    if (!ready || static_cast<int32_t>(now - nextAdmissionTry) >= 0) {
        nextAdmissionTry = now + 500;
        if (net::SendWorld(answer, 1) && ready && hostAdmissionAck) admitted = true;
    }
    net::SetInWorld(net::LocalId(), admitted);
    crashlog::WorldEntryWait(admitted ? nullptr : "host character / personal inventory / authoritative baselines / host admission ACK");
}
void OnWorld(uint8_t from, net::WorldNote const& note)
{
    if (net::IsHost()) {
        if (from < 2 || from >= net::MaxPlayers || note.epoch != epoch) return;
        if (note.op == net::WorldReady && clients[from].recordSent == epoch && ownCharacterReady && loading.empty()) {
            if (note.flags & net::WorldEntryConfirmed) {
                if (clients[from].cutover != epoch) return;
                if (clients[from].ready != epoch && (!story::FinishAdmission(from) || !doors::FinishAdmission(from))) return;
                const bool first = clients[from].ready != epoch;
                clients[from].ready = epoch;
                net::SetInWorld(from, true); // One-shot NPC/motion replays are now behind the client's applied cutover.
                if (first) Logf("persist: admission confirmed peer=%u account=%llu epoch=%08x baselines=1",
                                from, net::PeerAccount(from), epoch);
                return;
            }
            if (!story::FinishAdmission(from) || !doors::FinishAdmission(from)) return;
            const bool first = clients[from].cutover != epoch;
            clients[from].cutover = epoch;
            if (first) Logf("persist: admission cutover queued peer=%u account=%llu epoch=%08x; waiting for applied confirmation",
                            from, net::PeerAccount(from), epoch);
            net::WorldNote ack{};
            ack.op = net::WorldReady; ack.kind = localKind; ack.epoch = epoch;
            strncpy_s(ack.campaign, campaign.c_str(), _TRUNCATE);
            net::SendWorld(ack, from);
        } else if (note.op == net::WorldWaiting || note.op == net::WorldOther) net::SetInWorld(from, false);
        return;
    }
    if (note.op == net::WorldReady) {
        if (seenWelcome != net::WelcomeSerial() || note.epoch != hostWorld.epoch || note.kind == net::KindMenu || !ownCharacterReady ||
            synced != hostWorld.epoch || !story::AdmissionReady()) return;
        hostAdmissionAck = true;
        Evaluate();
        return;
    }
    if (note.op != net::WorldHost) return;
    if (note.epoch != hostWorld.epoch) {
        const bool replacing = hostWorld.epoch != 0 && hostWorld.kind != net::KindMenu;
        resetLocalWorld = replacing && gCSession::GetInstance().IsGameRunning();
        if (resetLocalWorld) {
            freshLocalHero = false;
            Notice(i18n::HostLoadsOnly, true);
        }
        captured = {}; requestPending = false; receivedRecord.clear(); receivedPersonal.clear(); receivedNote = {};
        needsNewHero = false; restoreFailed = false; synced = 0; ownCharacterReady = false; hostAdmissionAck = false;
        nextAdmissionTry = 0;
        net::SetInWorld(net::LocalId(), false);
        remote::DespawnAll("host world replaced");
        dialogaudio::OnWorldChange(); containers::OnWorldChange();
        story::OnWorldChange(); doors::OnWorldChange();
        journal::Clear();
    }
    hostWorld = note;
    Evaluate();
}
void HostTick(uint32_t now)
{
    if (!epoch) NewEpoch();
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        auto& client = clients[i];
        if (!net::PeerJoined(i)) { client = {}; continue; }
        const uint32_t generation = net::PeerGeneration(i);
        if (client.generation != generation) { client = {}; client.generation = generation; }
        if (client.ready == epoch) continue;
        if (static_cast<int32_t>(now - client.nextTry) < 0) continue;
        client.nextTry = now + 1000;
        net::WorldNote note{};
        note.op = net::WorldHost; note.kind = ownCharacterReady && loading.empty() && !restoreFailed ? localKind : net::KindMenu;
        note.epoch = epoch;
        strncpy_s(note.campaign, campaign.c_str(), _TRUNCATE);
        net::SendWorld(note, i); // Same reliable channel before the character.
        if (note.kind == net::KindMenu) continue;
        if (client.recordSent == epoch) continue; // ENet reliably delivers the accepted record; do not duplicate large payloads.
        const uint64_t account = net::PeerAccount(i);
        if (!account) { Logf("persist: join account unavailable peer=%u", i); continue; }
        if (!saving::Valid(current.key)) {
            current.key = {RandomId(), RandomId(), steam::PlayerId()};
            if (!saving::Valid(current.key)) { SaveFailure("identity"); return; }
        }
        auto const* record = saving::store::Find(current, saving::store::Character, account);
        Bytes envelope, state;
        if (record) {
            auto const* personal = saving::store::Find(current, saving::store::Personal, account);
            if (personal) state = personal->bytes;
            else if (!saving::personal::Encode({}, state)) { SaveFailure("personal-record"); return; }
            if (!saving::store::PackPlayer(record->bytes, state, envelope)) { SaveFailure("character-record"); return; }
        }
        if (SendRecord(i, net::CharacterRestore, {current.key.world, current.key.save, account},
                       envelope)) client.recordSent = epoch;
    }
}
bool ReadLegacy(std::string const& slot, Bytes const& sidecar, Save& save)
{
    if (sidecar.size() >= 5 && std::memcmp(sidecar.data(), "G3T 2", 5) == 0) {
        saving::store::LegacyManifest manifest;
        if (!saving::store::ParseLegacy(sidecar, manifest)) return false;
        for (int f = 0; f < 2; ++f) {
            Bytes bytes;
            if (!ReadAll(Path(slot, f), bytes) || bytes.size() != manifest.size[f] ||
                saving::store::Crc(bytes) != manifest.crc[f]) return false;
        }
        save.key = {manifest.key.world, manifest.key.save, steam::PlayerId()};
    } else if (!sidecar.empty() && !(sidecar.size() >= 5 && std::memcmp(sidecar.data(), "G3T 1", 5) == 0)) return false;
    if (!saving::Id(save.key.world) || !saving::Id(save.key.save)) save.key = {RandomId(), RandomId(), steam::PlayerId()};
    saving::store::Set(save, {saving::store::Journal, 1, 0, sidecar});
    // A plain native world is loadable before Steam publishes the host account; it grants no character authority.
    return saving::Id(save.key.world) && saving::Id(save.key.save);
}
bool RestoreJournal()
{
    auto const* block = saving::store::Find(current, saving::store::Journal);
    if (!block || block->bytes.empty()) { journal::Clear(); npcs::ClearCompanions(); downed::Restore(false, "plain-save"); return true; }
    const std::string path = Path(lastLoad, 2) + ".read";
    if (!WriteAtomic(path, block->bytes)) return false;
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") || !file) { DeleteFileA(path.c_str()); return false; }
    journal::Read(file); rewind(file); npcs::ReadCompanions(file); rewind(file);
    bool held = false;
    char line[256]{};
    while (fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "downed ", 7)) continue;
        int value = 0; char extra = 0;
        if (sscanf_s(line, "downed %d %c", &value, &extra, 1u) != 1 || (value != 0 && value != 1)) {
            fclose(file); DeleteFileA(path.c_str()); return false;
        }
        held = value != 0;
    }
    const bool ok = !ferror(file); fclose(file); DeleteFileA(path.c_str());
    if (ok) downed::Restore(held, "host-save");
    return ok;
}
bool __stdcall LoadDetour(void* self, bCString const* name)
{
    if (!pairedEnabled) return originalLoad && originalLoad(self, name);
    if (!net::IsHost()) { Notice(i18n::HostLoadsOnly, true); return false; }
    if (!NativeReady()) { SaveFailure("native-hooks"); return false; }
    if (!name || !SafeName(name->GetText()) || nativeLoadDepth || nativeSaveDepth || !trade::ReadyForSave()) {
        SaveFailure(!name || !SafeName(name->GetText()) ? "name" : !trade::ReadyForSave() ? "trade" : "loading"); return false;
    }
    const std::string slot = name->GetText();
    Logf("persist: host load requested slot=%s player=%llu", slot.c_str(), steam::PlayerId());
    if (saving::Prefix(slot, "G3T_pending_")) {
        Logf("persist: load refused slot=%s reason=uncommitted-staging", slot.c_str());
        Notice(i18n::SaveDamaged, true); return false;
    }
    Save save; Bytes bytes;
    const bool hasSidecar = Exists(Path(slot, 2));
    if (hasSidecar && !ReadAll(Path(slot, 2), bytes)) {
        Logf("persist: load refused slot=%s reason=container-read", slot.c_str());
        Notice(i18n::SaveDamaged, true); return false;
    }
    if (hasSidecar && bytes.empty()) {
        Logf("persist: load refused slot=%s reason=empty-container", slot.c_str());
        Notice(i18n::SaveDamaged, true); return false;
    }
    const bool managed = saving::store::Modern(bytes);
    if (managed) {
        if (!saving::store::Decode(bytes, save)) {
            Logf("persist: load refused slot=%s reason=container-format bytes=%zu", slot.c_str(), bytes.size());
            Notice(i18n::SaveDamaged, true); return false;
        }
        auto const* own = saving::store::Find(save, saving::store::Character, steam::PlayerId());
        if (!own || !ValidateCharacter(own->bytes, {save.key.world, save.key.save, steam::PlayerId()})) {
            SaveFailure("character-record"); return false;
        }
        if (!Materialize(slot, save)) {
            SaveFailure(saving::store::IoReason(lastIoFailure, "cache-publish-failed")); return false;
        }
    } else {
        if (Exists(Path(slot, 2) + ".publish")) {
            Logf("persist: load refused slot=%s reason=uncommitted-publication", slot.c_str());
            SaveFailure("publish-incomplete"); return false;
        }
        if (!ReadLegacy(slot, bytes, save)) {
            Logf("persist: load refused slot=%s reason=legacy-format-or-native-crc bytes=%zu", slot.c_str(), bytes.size());
            Notice(i18n::SaveDamaged, true); return false;
        }
        for (int f = 0; f < 2; ++f) {
            Bytes native;
            if (!ReadAll(Path(slot, f), native) || native.empty()) { SaveFailure("native-load"); return false; }
        }
    }
    saving::store::IoError markerError;
    if (managed && !saving::store::Remove(Path(slot, 2) + ".publish", &markerError)) {
        LogIo(markerError); SaveFailure(saving::store::IoReason(markerError, "cache-publish-failed")); return false;
    }
    if (self != FileManager(true)) { SaveFailure("native-load"); return false; }
    Logf("persist: load validated slot=%s managed=%d sidecar=%d save=%s blocks=%zu", slot.c_str(),
         managed ? 1 : 0, hasSidecar ? 1 : 0, save.key.save.c_str(), save.blocks.size());
    CancelSave("canceled-load"); // Only a validated replacement cancels an in-flight save.
    lastLoad = slot; loading = slot; current = std::move(save); campaign = slot;
    localKind = net::KindMenu; ownCharacterReady = false; restoreFailed = false;
    NewEpoch();
    ++loads; newWorld = false;
    dialogaudio::OnWorldChange(); containers::OnWorldChange(); remote::DespawnAll("host load");
    story::OnWorldChange(); doors::OnWorldChange(); journal::Clear();
    world::OnWorldDeactivate();
    lowerLoadCalls = 0; lowerLoadGood = true;
    ++nativeLoadDepth;
    crashlog::WorldEntryScope scope("native HUD.LoadGame");
    const bool hudOk = originalLoad(self, name);
    scope.Complete();
    --nativeLoadDepth;
    const bool ok = hudOk && lowerLoadCalls == 1 && lowerLoadGood;
    Logf("persist: native load returned slot=%s save=%s ok=%d hud_ok=%d lower_calls=%u lower_ok=%d managed=%d",
         slot.c_str(), current.key.save.c_str(), ok ? 1 : 0, hudOk ? 1 : 0, lowerLoadCalls, lowerLoadGood ? 1 : 0, managed ? 1 : 0);
    if (!ok) { loading.clear(); restoreFailed = true; SaveFailure("native-load"); return false; }
    receivedRecord.clear();
    receivedPersonal.clear();
    if (managed) receivedRecord = saving::store::Find(current, saving::store::Character, steam::PlayerId())->bytes;
    if (auto const* personal = saving::store::Find(current, saving::store::Personal, steam::PlayerId())) receivedPersonal = personal->bytes;
    pendingKey = {current.key.world, current.key.save, steam::PlayerId()};
    restoreAt = GetTickCount() + 100;
    return true;
}
bool ApplyCharacter(Bytes const& bytes, saving::Key const& key)
{
    const std::string base = CharacterPath();
    if (base.empty()) return false;
    const std::string path = base + ".restore";
    if (!WriteAtomic(path, bytes)) return false;
    const bool ok = hero::LoadRecord(path.c_str(), key);
    DeleteFileA(path.c_str());
    Logf("persist: character restore account=%llu save=%s bytes=%zu ok=%d reason=%s",
         key.player, key.save.c_str(), bytes.size(), ok ? 1 : 0, hero::LastRefusal() ? hero::LastRefusal() : "-");
    if (ok) ++recordsApplied;
    return ok;
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
        } else if (joined && needsNewHero) {
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
        return false;
    }
    ++processDepth; return true;
}
void EndFrame() { if (processDepth) --processDepth; }
bool NativeLoading() { return nativeLoadDepth != 0; }
bool HeroPending()
{
    return pairedEnabled && (NativeLoading() || !loading.empty() || !ownCharacterReady || needsNewHero || restoreFailed ||
                             (!net::IsHost() && !net::InWorld(net::LocalId()) &&
                              (!hostAdmissionAck || !story::AdmissionReady() || !doors::AdmissionReady())));
}
void OnWorldActivate()
{
    if (!loading.empty() || nativeSaveDepth) return;
    newWorld = true;
    freshLocalHero = true;
    if (net::IsHost()) { current = {}; campaign.clear(); ownCharacterReady = false; localKind = net::KindMenu; NewEpoch(); }
}
void OnWorldDeactivate()
{
    if (!loading.empty() || nativeSaveDepth) return;
    ownCharacterReady = false; localKind = net::KindMenu;
    if (net::IsHost()) { current = {}; campaign.clear(); NewEpoch(); }
    else net::SetInWorld(net::LocalId(), false);
}
void Tick()
{
    if (nativeLoadDepth || nativeSaveDepth) return;
    const uint32_t now = GetTickCount();
    RetryCleanup(now);
    RetryReplies(now); StatusHints();
    auto& game = gCSession::GetInstance();
    const bool running = game.GetHero() && game.IsGameRunning();
    if (newWorld && running) {
        newWorld = false; localKind = net::KindNew;
        resetLocalWorld = false;
        // Only a freshly initialized local hero is permitted when the host has no account record.
        if (net::IsHost()) {
            ownCharacterReady = true; current.key = {RandomId(), RandomId(), steam::PlayerId()}; NewEpoch();
        } else if (needsNewHero) {
            needsNewHero = false; freshLocalHero = false; ownCharacterReady = true; synced = hostWorld.epoch;
            story::PrepareAdmission(); doors::PrepareAdmission(); Evaluate();
        }
    }
    if (net::IsHost()) {
        PollSave(now);
        if (!loading.empty() && running && static_cast<int32_t>(now - restoreAt) >= 0) {
            if (!PersonalReadyForRestore(receivedPersonal)) {
                if (now - restoreAt > 10000) { restoreFailed = true; loading.clear(); SaveFailure("personal-hooks"); }
                HostTick(now); return;
            }
            const bool ownOk = receivedRecord.empty() || ApplyCharacter(receivedRecord, pendingKey);
            const char* refusal = !ownOk ? "character-record" : !containers::RestorePersonal(receivedPersonal) ? "personal-record" :
                                  !RestoreJournal() ? "journal-write" :
                                  !RestoreExtensions() ? "extension" : nullptr;
            if (refusal) {
                restoreFailed = true; loading.clear(); SaveFailure(refusal); return;
            }
            receivedRecord.clear(); receivedPersonal.clear(); loading.clear(); ownCharacterReady = true; localKind = net::KindSaved;
            Logf("persist: host load settled save=%s epoch=%08x own_character=1", current.key.save.c_str(), epoch);
        }
        HostTick(now);
        return;
    }
    if (!net::Joined()) { captured = {}; requestPending = false; return; }
    if (seenWelcome != net::WelcomeSerial()) {
        seenWelcome = net::WelcomeSerial(); synced = 0;
        hostAdmissionAck = false; nextAdmissionTry = 0;
        if (receivedWelcome != seenWelcome) { receivedRecord.clear(); receivedPersonal.clear(); receivedNote = {}; }
        // An admitted same-epoch reconnect keeps its own live character; a new host epoch always restores the save.
        if (ownCharacterReady && hostWorld.epoch && !needsNewHero) {
            synced = hostWorld.epoch; story::PrepareAdmission(); doors::PrepareAdmission(); Evaluate();
        }
    }
    if (requestPending) {
        if (requestWelcome != net::WelcomeSerial() || requested.epoch != hostWorld.epoch || now - requestStarted > 10000) {
            if (requestWelcome == net::WelcomeSerial() && requested.epoch == hostWorld.epoch) {
                auto cancel = requested; cancel.op = net::SaveCancel; cancel.ok = 0; cancel.world[0] = 0;
                strncpy_s(cancel.name, "request-timeout", _TRUNCATE); QueueReply(1, cancel);
            }
            requestPending = false; SaveFailure("request-timeout");
        } else if (static_cast<int32_t>(now - requestRetry) >= 0) {
            requestRetry = now + 500; net::SendSave(1, requested);
        }
    }
    if (captured.valid) {
        if (captured.welcome != net::WelcomeSerial() || captured.note.epoch != hostWorld.epoch || now - captured.started > 15000) {
            captured.valid = false; captured.bytes.clear(); SaveFailure("result-timeout");
        } else if (!captured.queued && static_cast<int32_t>(now - captured.nextTry) >= 0) {
            captured.nextTry = now + 500;
            captured.queued = SendRecord(1, net::CharacterCapture, {captured.note.world, captured.note.save, steam::PlayerId()}, captured.bytes);
        }
    }
    if (!receivedRecord.empty() && !restoreFailed && !resetLocalWorld && running && receivedWelcome == net::WelcomeSerial() &&
        receivedNote.epoch == hostWorld.epoch && static_cast<int32_t>(now - restoreAt) >= 0) {
        if (!PersonalReadyForRestore(receivedPersonal)) {
            if (now - restoreAt > 10000) { restoreFailed = true; SaveFailure("personal-hooks"); }
            return;
        }
        if (!ApplyCharacter(receivedRecord, {receivedNote.world, receivedNote.save, steam::PlayerId()})) {
            restoreFailed = true; SaveFailure("character-record"); return;
        }
        if (!containers::RestorePersonal(receivedPersonal)) {
            restoreFailed = true; SaveFailure("personal-record"); return;
        }
        receivedRecord.clear(); receivedPersonal.clear(); ownCharacterReady = true; synced = hostWorld.epoch;
        story::PrepareAdmission(); doors::PrepareAdmission(); Evaluate();
    }
    if (ownCharacterReady && !net::InWorld(net::LocalId()) && synced == hostWorld.epoch &&
        story::AdmissionReady() && doors::AdmissionReady()) Evaluate();
}

namespace SaveExtensions {
bool Register(char const* name, uint32_t type, uint32_t version, Capture capture, Restore restore)
{
    if (!name || !*name || std::strlen(name) > 48 || type < saving::store::Extension || !version || !capture || !restore ||
        extensions.size() >= 32 || std::any_of(extensions.begin(), extensions.end(), [=](auto const& e) {
            return e.type == type || e.name == name;
        })) {
        Logf("persist: extension registration refused name=%s type=%u version=%u", name ? name : "-", type, version);
        return false;
    }
    extensions.push_back({name, type, version, capture, restore});
    return true;
}
}
void Install()
{
    if (installed) return;
    net::SetSaveHandler(&OnSave);
    net::SetCharacterHandler(&OnCharacter);
    net::SetWorldHandler(&OnWorld);
    if (!SaveExtensions::Register("npclease", NpcLeaseExtension, 1, &CaptureNpcLease, &RestoreNpcLease))
        Log("persist: NPC lease save overlay registration failed; lease gate remains active");
    unsigned char* resolved[5]{};
    if (!ResolveNative(resolved)) {
        Log("persist: save/load hooks not installed; host-managed saves off");
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
    originalDelete = reinterpret_cast<DeleteFn>(ai::JumpTo(deleteTarget, reinterpret_cast<void*>(&DeleteDetour), Stolen));
    uiCallInstalled = HookNativeCall(uiEntries[2] + 6, uiEntries[1], reinterpret_cast<void*>(&MenuNewSlot), "host menu-save call");
    pairedEnabled = originalSave && originalLoad && originalWorldSave && originalWorldLoad && loadCallInstalled &&
                    originalGuiMessage && uiCallInstalled && originalDelete;
    if (pairedEnabled) pairedEnabled = ReverifyNative();
    if (originalSave) saveTarget = resolved[0];
    if (originalLoad) loadTarget = resolved[1];
    installed = true;
    if (!pairedEnabled) {
        Logf("persist: save/load hook installation failed SaveGameFn=%d LoadGameFn=%d WorldSaveGameFn=%d WorldLoadGameFn=%d; partial hooks pass through, host-managed saves off",
             originalSave != nullptr, originalLoad != nullptr, originalWorldSave != nullptr, loadCallInstalled ? 1 : 0);
        return;
    }
    Logf("persist: save and load hooked SaveGameFn=%p LoadGameFn=%p WorldSaveGameFn=%p WorldLoadGameFn=%p FileManagerGetter=%p protocol=%u",
         resolved[0], resolved[1], resolved[2], resolved[3], resolved[4], saving::Protocol);
    Logf("persist: host-managed save UI hooked GUI.HandleMessage=%p Menu.NewSaveSlotCall=%p Session.IsSaveAllowed=%p HUD.CaptureSaveImage=%p",
         uiEntries[0], uiEntries[2] + 6, uiEntries[3], uiEntries[4]);
}
void* SaveTarget() { return saveTarget; }
void* LoadTarget() { return loadTarget; }
bool CheckpointBusy() { return operation.active || captured.valid || requestPending || nativeSaveDepth; }
void Off(LogFn log) { writing = false; log("PERSISTOFF ok=1"); }
void CampaignOff(LogFn log) { log("CAMPAIGNOFF retired=1 (no world transfer)"); }
void WorldSyncOff(LogFn log) { log("WORLDSYNCOFF retired=1"); }
void GenerationOff(LogFn log) { log("GENERATIONOFF retired=1"); }
void WorldGateLate(LogFn log) { log("WORLDGATELATE retired=1"); }
void RecordWaitOff(LogFn log) { log("RECORDWAITOFF retired=1"); }
void Pause(char const*, LogFn log) { log("PERSISTPAUSE retired=1"); }
void BadTransfer(LogFn log) { log("BADTRANSFER retired=1 (no world transfer)"); }
void Waiting(LogFn log) {
    char line[128]; Format(line, "RECORDWAIT waiting=%d", HeroPending() ? 1 : 0); log(line);
}
void LoadTicksOff(char const*, LogFn log) { log("LOADTICKSOFF retired=1"); }
void WorldReport(LogFn log) {
    char line[256]; Format(line, "WORLD role=%s epoch=%08x host_epoch=%08x own=%d save_id=%s",
        net::IsHost() ? "host" : "client", epoch, hostWorld.epoch, ownCharacterReady ? 1 : 0, current.key.save.c_str()); log(line);
}
void Report(LogFn log) {
    char line[256]; Format(line, "PERSIST hooked=%d saves=%u loads=%u records_written=%u records_applied=%u active=%d network_world_bytes=0",
        pairedEnabled ? 1 : 0, saves, loads, recordsWritten, recordsApplied, operation.active ? 1 : 0); log(line);
}
bool Switch(char const* action, LogFn log) {
    if (!std::strcmp(action, "resendguardoff") || !std::strcmp(action, "lastownherooff")) {
        log("SWITCH retired=1"); return true;
    }
    return false;
}
}
