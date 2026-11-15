// SPDX-License-Identifier: GPL-3.0-or-later
// Shared internal state and helpers across persist*.cpp translation units.
#pragma once

#include "persist.h"

#include "client_autoload.h"
#include "client_rejoin.h"
#include "client_template_store.h"
#include "hud.h"
#include "i18n.h"
#include "net.h"
#include "saving_native.h"
#include "saving_policy.h"
#include "saving_store.h"
#include "savethumb.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace g3t::persist::detail {

constexpr size_t Stolen = saving::native::Stolen;
constexpr UINT_PTR SaveTimer = 0x47335453;
constexpr uint32_t LegacyNpcLeaseExtension = saving::store::Extension;

using FileFn = bool(__stdcall*)(void*, bCString const*);
using Getter = void*(__cdecl*)();
using WorldSaveFn = bool(__thiscall*)(gCWorld*, bCString const&, eCGfxImageData*, void*, GEU32);
using WorldLoadFn = bool(__thiscall*)(gCWorld*, bCString const&, bool);
using GuiMessageFn = bool(__thiscall*)(gCGUIManager*, GEUInt, GEUInt, GEInt);
using NewSlotFn = bool(__stdcall*)(void*);
using DeleteFn = bool(__thiscall*)(gCWorld*, bCString const&);
using NewGameCommandFn = void(__thiscall*)(void*);
using NewGameFn = bool(__stdcall*)(void*);
using ConfirmedStartFn = bool(__cdecl*)();
using Bytes = saving::store::Bytes;
using Save = saving::store::Save;

enum class TemplateJob { None, FirstEntry, Background, HostLoad };

struct PendingCleanup {
    std::string path;
    uint32_t until = 0, nextTry = 0;
};

struct Client {
    uint32_t generation = 0, ready = 0, recordSent = 0, nextTry = 0, cutover = 0;
    uint32_t waitingSince = 0;
    bool told = false;
    bool hasSavedPose = false;
    saving::RecordPose savedPose{};
};

struct Captured {
    net::SaveNote note{};
    Bytes bytes;
    uint32_t welcome = 0, started = 0, nextTry = 0;
    bool valid = false;
    bool queued = false;
};

struct Reply {
    net::SaveNote note{};
    uint32_t generation = 0, nextTry = 0, expires = 0;
    bool pending = false;
};

struct RequestCache {
    std::string token, transaction;
    net::SaveNote result{};
    uint32_t generation = 0, epoch = 0;
    bool complete = false;
};

struct ExtensionEntry {
    std::string name;
    uint32_t type = 0, version = 0;
    SaveExtensions::Capture capture = nullptr;
    SaveExtensions::Restore restore = nullptr;
};

extern DeleteFn originalDelete;
extern unsigned char* deleteTarget;
extern unsigned lowerSaveCalls;
extern bool lowerSaveGood;
extern GuiMessageFn originalGuiMessage;
extern NewSlotFn originalNewSlot;
extern unsigned char* uiEntries[5];
extern bool uiCallInstalled;
extern unsigned saveRequestsSeen;
extern FileFn originalSave, originalLoad;
extern Getter fileManagerGetter;
extern void* fileManagerSingleton;
extern void* fileManagerVtable;
extern WorldSaveFn originalWorldSave;
extern WorldLoadFn originalWorldLoad;
extern unsigned lowerLoadCalls;
extern bool lowerLoadGood;
extern unsigned char* saveTarget;
extern unsigned char* loadTarget;
extern unsigned char* nativeEntries[5];
extern unsigned char* nativeImage;
extern size_t nativeImageSize;
extern bool loadCallInstalled;
extern bool installed, pairedEnabled, writing;
extern unsigned processDepth, nativeLoadDepth, nativeSaveDepth;
extern unsigned saves, loads;
extern unsigned recordsWritten, recordsApplied, loadFrames, epochs;
extern std::string lastSave, lastLoad, campaign, loading;
extern std::string hookFailure, pupDigestHex;
extern uint8_t localKind;
extern bool newWorld, ownCharacterReady, needsNewHero, restoreFailed;
extern bool freshLocalHero, resetLocalWorld;
extern rejoin::State clientRejoin;
extern NewGameCommandFn originalNewGameCommand;
extern NewGameFn originalNewGame;
extern void (*worldCleanup)();
extern bool rejoinHooks, nativeNewGame, requestCharacterAgain;
extern bool clientWorldEntered;
extern rejoin::tmpl::Capture templateCapture;
extern bool templateHeroFresh;
extern uint32_t recordWaitSince;
extern bool recordWaitGaveUp;
extern autoload::Controller autoloadState;
extern autoload::Status autoloadStatus;
extern uint32_t autoloadRemainingMs, menuButtonSeen, routeCheckedAt;
extern bool entryManual, entryApproved, autoloadAwaiting, routeAvailable, escWasDown;
extern ConfirmedStartFn confirmedStart;
extern unsigned menuCommandDepth;
extern rejoin::Wait rejoinWait;
extern bool hostAdmissionAck;
extern uint32_t nextAdmissionTry;
extern uint32_t seenWelcome;
extern uint32_t epoch, synced;
extern net::WorldNote hostWorld;
extern Save current, savingManifest;
extern std::string requestedSlot, stagingSlot, lastSaveFailure;
extern bool nativeSaved;
extern uint32_t nativeSavedAt;
extern saving::store::IoError lastIoFailure;
extern std::vector<PendingCleanup> pendingCleanup;
extern saving::Key pendingKey;
extern HWND saveWindow;
extern saving::thumb::Tracker thumbnail;
extern saving::CaptureRound operation;
extern Client clients[net::MaxPlayers];
extern Captured captured;
extern net::SaveNote requested;
extern uint32_t requestWelcome, requestStarted, requestRetry;
extern bool requestPending;
extern Reply hostReplies[net::MaxPlayers], clientReply;
extern RequestCache requests[net::MaxPlayers];
extern std::vector<ExtensionEntry> extensions;
extern uint32_t restoreAt, receivedWelcome;
extern Bytes receivedRecord, receivedPersonal;
extern net::CharacterNote receivedNote;
extern rejoin::tmpl::Publish templatePublish;
extern rejoin::tmpl::Background templateBackground;
extern TemplateJob templateJob;
extern unsigned templateFailures, templateAttempts;
extern std::string templateFailCode, templateSettleShown;
extern uint32_t templateFailError, templateIoError, templateNextRecover;

// Native PE/PUP verification & call-edge hooks (persist_hooks.cpp)
bool ResolveNative(unsigned char* (&resolved)[5]);
void* FileManager(bool forLoad);
bool PupLoadEntry(unsigned char* entry, bool beforeUse = false);
bool ReverifyNative(bool beforeUse = false);
bool NativeReady();
bool HookNativeCall(unsigned char* call, void* entry, void* detour, char const* label);
bool HookWorldLoadCall(unsigned char* call, unsigned char* entry);
bool __fastcall WorldLoadDetour(gCWorld* self, void*, bCString const& name, bool nativeFlag);

// Slot storage, character/personal/journal/extension serialization, & LoadDetour (persist_storage.cpp)
bool SafeName(char const* name);
std::string Folder();
char const* SaveExtension(int file);
std::string Path(std::string const& slot, int file);
bool Exists(std::string const& path);
void LogIo(saving::store::IoError const& error);
bool ReadAll(std::string const& path, std::vector<uint8_t>& out);
bool WriteAtomic(std::string const& path, Bytes const& bytes);
uint64_t GameModuleBytes();
std::string HexId(uint8_t const* bytes);
std::string RandomId();
std::string CharacterPath();
bool ValidateCharacter(Bytes const& bytes, saving::Key const& key);
bool CaptureCharacter(saving::Key const& key, Bytes& bytes);
bool CapturePersonalWithFallback(uint64_t account, Bytes& state);
bool CapturePlayer(saving::Key const& key, Bytes& bytes, char const*& reason);
bool PersonalReadyForRestore(Bytes const& bytes);
bool ApplyCharacter(Bytes const& bytes, saving::Key const& key);
net::SaveNote Note(uint8_t op, Save const& save, bool ok = true);
void QueueReply(uint8_t to, net::SaveNote const& note);
void RetryReplies(uint32_t now);
bool SendRecord(uint8_t to, uint8_t purpose, saving::Key const& key, Bytes const& bytes);
void StopSaveTimer();
void CleanupStaging();
void RetryCleanup(uint32_t now);
void FinishSave(bool ok, char const* why);
void CancelSave(char const* why);
bool AllReady();
bool CaptureJournal(Bytes& bytes);
bool CaptureExtensions(Save& save);
bool RestoreExtensions();
bool Materialize(std::string const& slot, Save const& save);
bool TracePublish(saving::store::PublishStage stage, saving::store::IoError&);
bool __fastcall DeleteDetour(gCWorld* self, void*, bCString const& name);
void PollSave(uint32_t now);
void OnCharacter(uint8_t from, net::CharacterNote const& note, Bytes const& bytes);
bool ReadLegacy(std::string const& slot, Bytes const& sidecar, Save& save);
bool RestoreJournal();
bool __stdcall LoadDetour(void* self, bCString const* name);
void HostNotice(i18n::Message message, uint8_t peer, hud::Kind kind);

// Client rejoin template, autoload & transition (persist_client.cpp)
bool AutoloadAvailable();
bool HooksUnsupported();
void HostNameWide(wchar_t (&out)[80]);
wchar_t const* UnsupportedText(wchar_t* buffer, size_t size);
void AutoloadCancel(char const* by, bool notify = false);
void QueueClientReturn();
void __fastcall NewGameCommandDetour(void* self, void*);
bool __stdcall NewGameDetour(void* self);
rejoin::tmpl::store::Paths TemplatePaths();
rejoin::tmpl::Identity TemplateIdentity();
void TemplateTrace(char const* step, std::string const& path, uint32_t code, bool ok);
void TemplateInvalidate();
rejoin::tmpl::Report TemplateState(bool deep = false);
bool RemoveTemplate(char const* reason);
char const* TemplateJobName(TemplateJob job);
void TemplateNotify(char const* code, uint32_t error);
rejoin::Boundary ClientBoundary();
void TemplateEnded(uint32_t now);
void TemplatePoll(uint32_t now);
bool TemplateBackgroundDue(uint32_t now);
wchar_t const* AutoloadMenuText(wchar_t* buffer, size_t size);

// Core save entry points, world epoch/admission, & notices (persist.cpp)
char const* NativeSaveReason();
char const* SaveReadiness();
bool __fastcall GuiMessageDetour(gCGUIManager* self, void*, GEUInt message, GEUInt key, GEInt flags);
bool __stdcall MenuNewSlot(void* self);
bool __fastcall WorldSaveDetour(gCWorld* self, void*, bCString const& name, eCGfxImageData* image, void* metadata, GEU32 size);
bool __stdcall SaveDetour(void* self, bCString const* name);
void Notice(i18n::Message message, bool bad = false);
void NoticeText(wchar_t const* text, hud::Kind kind);
void NoticeText(wchar_t const* text, bool bad);
void SaveFailure(char const* code);
void ClientRestoreFailure(char const* code);
bool TemplateStart(rejoin::tmpl::Origin origin, TemplateJob job);
void OnSave(uint8_t from, net::SaveNote const& note);
void NewEpoch();
void Evaluate();
void OnWorld(uint8_t from, net::WorldNote const& note);
void HostTick(uint32_t now);

} // namespace g3t::persist::detail
