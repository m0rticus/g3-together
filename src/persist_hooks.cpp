// SPDX-License-Identifier: GPL-3.0-or-later
// Native Game.dll and PUP companion (paru_s2.dll) PE verification and save/load call-edge hooks.
#include "persist_internal.h"

#include "ai.h"
#include "clock.h"
#include "crashlog.h"
#include "doors.h"
#include "journal.h"
#include "log.h"
#include "npcs.h"
#include "steam.h"
#include "story.h"

#include <bcrypt.h>
#include <algorithm>
#include <cstring>

namespace g3t::persist::detail {

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
bool installed = false, pairedEnabled = false;
unsigned processDepth = 0, nativeLoadDepth = 0, nativeSaveDepth = 0;
unsigned epochs = 0;
std::string lastLoad, campaign, loading;
std::string hookFailure, pupDigestHex;
uint8_t localKind = net::KindMenu;
bool newWorld = false, ownCharacterReady = false, needsNewHero = false, restoreFailed = false;
bool resetLocalWorld = false;
rejoin::State clientRejoin;
NewGameCommandFn originalNewGameCommand = nullptr;
NewGameFn originalNewGame = nullptr;
void (*worldCleanup)() = nullptr;
bool rejoinHooks = false, nativeNewGame = false, requestCharacterAgain = false;
bool clientWorldEntered = false;
rejoin::tmpl::Capture templateCapture;
bool templateHeroFresh = true;
uint32_t recordWaitSince = 0;
bool recordWaitGaveUp = false;
autoload::Controller autoloadState;
autoload::Status autoloadStatus = autoload::Status::Off;
uint32_t autoloadRemainingMs = 0, menuButtonSeen = 0, routeCheckedAt = 0;
bool entryManual = false, entryApproved = false, autoloadAwaiting = false, routeAvailable = false, escWasDown = false;
ConfirmedStartFn confirmedStart = nullptr;
unsigned menuCommandDepth = 0;
rejoin::Wait rejoinWait = rejoin::Wait::None;
bool hostAdmissionAck = false;
uint32_t nextAdmissionTry = 0;
uint32_t seenWelcome = 0;
uint32_t epoch = 0, synced = 0;
net::WorldNote hostWorld{};
Save current, savingManifest;
std::string requestedSlot, stagingSlot, lastSaveFailure;
bool nativeSaved = false;
uint32_t nativeSavedAt = 0;
saving::store::IoError lastIoFailure;
std::vector<PendingCleanup> pendingCleanup;
saving::Key pendingKey;
HWND saveWindow = nullptr;
saving::thumb::Tracker thumbnail;
saving::CaptureRound operation;
Client clients[net::MaxPlayers]{};
Captured captured;
net::SaveNote requested{};
uint32_t requestWelcome = 0, requestStarted = 0, requestRetry = 0;
bool requestPending = false;
Reply hostReplies[net::MaxPlayers], clientReply;
RequestCache requests[net::MaxPlayers]{};
std::vector<ExtensionEntry> extensions;
uint32_t restoreAt = 0, receivedWelcome = 0;
Bytes receivedRecord, receivedPersonal;
net::CharacterNote receivedNote{};
rejoin::tmpl::Publish templatePublish;
rejoin::tmpl::Background templateBackground;
TemplateJob templateJob = TemplateJob::None;
unsigned templateFailures = 0, templateAttempts = 0;
std::string templateFailCode, templateSettleShown;
uint32_t templateFailError = 0, templateIoError = 0, templateNextRecover = 0;

namespace {

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
    if (hookFailure.empty()) hookFailure = name;
    char bytes[25] = "unreadable";
    if (Readable(entry, 12))
        for (size_t b = 0; b < 12; ++b) Format(bytes + b * 2, 3, "%02x", entry[b]);
    Logf("persist: unresolved %s phase=%s live=%s; host-managed saves off", name, phase, bytes);
}

void LogPupIdentity(HMODULE module, bool pinned, bool emit = true)
{
    struct Identity { HMODULE module; std::string digest; bool logged; };
    static std::vector<Identity> identities;
    size_t index = 0;
    while (index < identities.size() && identities[index].module != module) ++index;
    if (index == identities.size()) {
        Identity identity{module, {}, false};
        char path[MAX_PATH]{};
        std::vector<uint8_t> bytes;
        const DWORD length = module ? GetModuleFileNameA(module, path, MAX_PATH) : 0;
        if (length && length < MAX_PATH && ReadAll(path, bytes) && bytes.size() <= UINT32_MAX) {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            uint8_t digest[32]{};
            NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
            if (status >= 0) status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
            if (status >= 0 && !bytes.empty()) status = BCryptHashData(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0);
            if (status >= 0) status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            if (status >= 0) {
                char hex[65]{};
                for (size_t i = 0; i < sizeof(digest); ++i) Format(hex + i * 2, 3, "%02x", digest[i]);
                identity.digest = hex;
            }
        }
        identities.push_back(identity);
    }
    auto& identity = identities[index];
    pupDigestHex = identity.digest;
    if (emit && !identity.logged) {
        Logf("persist: PUP identity digest=%s verified=%s",
             identity.digest.empty() ? "unavailable" : identity.digest.c_str(), pinned ? "pinned" : "structural");
        identity.logged = true;
    }
}

bool PupRefused(char const* stage, unsigned char* entry, uint32_t bridge, uint32_t receiver, bool beforeUse)
{
    if (hookFailure.empty()) hookFailure = std::string("pup:") + stage;
    const uintptr_t observed = receiver ? receiver : bridge ? bridge : reinterpret_cast<uintptr_t>(entry);
    auto* at = reinterpret_cast<unsigned char const*>(observed);
    char bytes[19] = "unreadable";
    if (Readable(at, 9))
        for (size_t i = 0; i < 9; ++i) Format(bytes + i * 2, 3, "%02x", at[i]);
    Logf("persist: unresolved WorldLoadGameFn PUP phase=%s stage=%s entry=%p bridge=%08x receiver=%08x observed=%p live=%s; host-managed saves off",
         beforeUse ? "before-use" : "install", stage, entry, bridge, receiver, at, bytes);
    return false;
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

enum class PeStatus { Ok, Memory, Header, Layout };

PeStatus ValidatePe32Image(unsigned char* image, size_t maxImageSize, IMAGE_NT_HEADERS32*& outNt,
                           IMAGE_SECTION_HEADER*& outSections)
{
    outNt = nullptr;
    outSections = nullptr;
    if (!Readable(image, sizeof(IMAGE_DOS_HEADER))) return PeStatus::Memory;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096 ||
        !Readable(image + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS32)))
        return PeStatus::Header;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || !size ||
        (maxImageSize && size > maxImageSize) ||
        !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96 ||
        !Readable(sections, nt->FileHeader.NumberOfSections * sizeof(*sections)))
        return PeStatus::Layout;
    outNt = nt;
    outSections = sections;
    return PeStatus::Ok;
}

} // namespace

bool ResolveNative(unsigned char* (&resolved)[5])
{
    HMODULE game = GetModuleHandleA("Game.dll");
    auto* image = reinterpret_cast<unsigned char*>(game);
    IMAGE_NT_HEADERS32* nt = nullptr;
    IMAGE_SECTION_HEADER* sections = nullptr;
    const PeStatus pe = ValidatePe32Image(image, 128u * 1024 * 1024, nt, sections);
    if (pe != PeStatus::Ok) {
        Log(pe == PeStatus::Memory ? "persist: unresolved Game.dll; host-managed saves off" :
            pe == PeStatus::Header ? "persist: unresolved Game.dll PE header; host-managed saves off" :
                                     "persist: unresolved Game.dll PE layout; host-managed saves off");
        return false;
    }
    const size_t size = nt->OptionalHeader.SizeOfImage;
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
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(receiver), &pup) || !pup)
        return PupRefused("receiver-module", entry, bridge, receiver, beforeUse);
    LogPupIdentity(pup, false, false);
    const auto refuse = [&](char const* stage) {
        LogPupIdentity(pup, false);
        return PupRefused(stage, entry, bridge, receiver, beforeUse);
    };
    auto* image = reinterpret_cast<unsigned char*>(pup);
    IMAGE_NT_HEADERS32* nt = nullptr;
    IMAGE_SECTION_HEADER* sections = nullptr;
    const PeStatus pe = ValidatePe32Image(image, 0, nt, sections);
    if (pe != PeStatus::Ok)
        return refuse(pe == PeStatus::Memory ? "companion-memory" :
                      pe == PeStatus::Header ? "companion-header" : "companion-layout");
    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(image);
    const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (receiver < imageBase || receiver - imageBase >= imageSize)
        return refuse("receiver-rva");
    const uint32_t receiverRva = static_cast<uint32_t>(receiver - imageBase);
    unsigned matches = 0;
    if (FindCode(image, imageSize, sections, nt->FileHeader.NumberOfSections,
                 saving::native::PupReceiverBytes, matches) != receiverRva)
        return refuse("receiver-pattern");
    auto const* code = reinterpret_cast<uint8_t const*>(receiver);
    saving::native::PupLayout layout{};
    if (!Readable(code, 41) || !Executable(code) ||
        !saving::native::PupReceiverLayoutAt(code, 41, receiver, static_cast<uint32_t>(imageBase), imageSize, layout))
        return refuse("receiver-operands");
    LogPupIdentity(pup, saving::native::PupLayoutIsPinned(receiverRva, imageSize, layout));
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
    if (!saving::native::Range(imageSize, layout.waitImport, 4) ||
        !saving::native::Range(imageSize, layout.releaseImport, 4) ||
        !saving::native::Range(imageSize, layout.adapter, 4) ||
        !saving::native::Range(imageSize, layout.semaphore, 4) ||
        !Readable(image + layout.waitImport, 4) || !Readable(image + layout.releaseImport, 4) ||
        !Readable(image + layout.adapter, 4) || !Readable(image + layout.semaphore, 4))
        return PupRefused("runtime-memory", entry, bridge, receiver, beforeUse);
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    if (saving::native::Word(image, layout.waitImport) != reinterpret_cast<uintptr_t>(GetProcAddress(kernel, "WaitForSingleObject")) ||
        saving::native::Word(image, layout.releaseImport) != reinterpret_cast<uintptr_t>(GetProcAddress(kernel, "ReleaseSemaphore")))
        return PupRefused("runtime-imports", entry, bridge, receiver, beforeUse);
    const uintptr_t original = saving::native::Word(image, layout.adapter);
    if (!original || original == reinterpret_cast<uintptr_t>(entry) || original == bridge || original == receiver ||
        !Executable(reinterpret_cast<void const*>(original)))
        return PupRefused("runtime-adapter", entry, bridge, receiver, beforeUse);
    DWORD flags = 0;
    if (!GetHandleInformation(reinterpret_cast<HANDLE>(saving::native::Word(image, layout.semaphore)), &flags))
        return PupRefused("runtime-semaphore", entry, bridge, receiver, beforeUse);
    if (readyBridge != bridge || readyReceiver != receiver) {
        readyBridge = bridge;
        readyReceiver = receiver;
        Logf("persist: WorldLoadGameFn PUP ready entry=%p bridge=%08x receiver=%08x owner=%p adapter=%p",
             entry, bridge, receiver, image, reinterpret_cast<void*>(original));
    }
    return true;
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
    rejoinHooks = false;
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

void NoticeText(wchar_t const* text, hud::Kind kind)
{
    auto& game = gCSession::GetInstance();
    auto* manager = game.GetGUIManager();
    if (!game.IsGameRunning() || (manager && manager->IsMenuOpen())) hud::MenuLine(text);
    else hud::Message(kind, text);
}

void NoticeText(wchar_t const* text, bool bad)
{
    NoticeText(text, bad ? hud::Warning : hud::Info);
}

void Notice(i18n::Message message, bool bad)
{
    NoticeText(i18n::Text(message), bad);
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

void ClientRestoreFailure(char const* code)
{
    Logf("persist: client character restore failed reason=%s campaign=%s save=%s player=%llu", code,
         *hostWorld.campaign ? hostWorld.campaign : "-", receivedNote.save, steam::PlayerId());
    wchar_t label[80] = {}, text[500] = {};
    if (*hostWorld.campaign) MultiByteToWideChar(CP_UTF8, 0, hostWorld.campaign, -1, label, 79);
    _snwprintf_s(text, _TRUNCATE, i18n::Text(i18n::RecordRefusedWaiting), *label ? label : i18n::SaveReason(code));
    NoticeText(text, true);
}

} // namespace g3t::persist::detail

namespace g3t::persist {
using namespace detail;

void* SaveTarget() { return saveTarget; }
void* LoadTarget() { return loadTarget; }

bool BeginFrame()
{
    if (nativeLoadDepth || nativeSaveDepth) {
        crashlog::WorldEntryWait(nativeLoadDepth ? "native HUD.LoadGame return" : "native save return");
        return false;
    }
    ++processDepth;
    return true;
}

void EndFrame()
{
    if (processDepth) --processDepth;
}

bool NativeLoading() { return nativeLoadDepth != 0; }

bool CheckpointBusy()
{
    return operation.active || captured.valid || requestPending || nativeSaveDepth;
}

bool HeroPending()
{
    // A client whose hooks are off can never be admitted: it is pending forever, never "playing as if admitted".
    return (pairedEnabled || !net::IsHost()) &&
           (NativeLoading() || clientRejoin.pending || !loading.empty() || !ownCharacterReady || needsNewHero || restoreFailed ||
                             (!net::IsHost() && !net::InWorld(net::LocalId()) &&
                              (!hostAdmissionAck || !story::AdmissionReady() || !doors::AdmissionReady())));
}

void OnWorldActivate()
{
    if (!loading.empty() || nativeSaveDepth) return;
    if (!net::IsHost()) return; // Only successful HUD.NewGame authorizes a new client hero.
    newWorld = true;
    current = {};
    campaign.clear();
    ownCharacterReady = false;
    needsNewHero = false;
    restoreFailed = false;
    localKind = net::KindMenu;
    journal::Clear();
    npcs::ClearCompanions();
    clock::OnWorldChange();
    NewEpoch();
}

void OnWorldDeactivate()
{
    if (!loading.empty() || nativeSaveDepth) return;
    ownCharacterReady = false;
    localKind = net::KindMenu;
    if (net::IsHost()) {
        current = {};
        campaign.clear();
        needsNewHero = false;
        restoreFailed = false;
        journal::Clear();
        npcs::ClearCompanions();
        clock::OnWorldChange();
        NewEpoch();
    } else {
        net::SetInWorld(net::LocalId(), false);
        if (nativeNewGame || clientRejoin.pending) return;
        clock::OnWorldChange();
        newWorld = false;
        resetLocalWorld = false;
        entryManual = false;
        entryApproved = false;
        autoloadAwaiting = false;
        clientRejoin.Left();
        receivedRecord.clear();
        receivedPersonal.clear();
        receivedNote = {};
        receivedWelcome = 0;
        synced = 0;
        hostAdmissionAck = false;
        needsNewHero = false;
        restoreFailed = false;
        requestCharacterAgain = true;
    }
}
} // namespace g3t::persist
