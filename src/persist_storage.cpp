// SPDX-License-Identifier: GPL-3.0-or-later
// Slot paths, atomic I/O, character/personal/journal/extension records, save round transport, and host LoadDetour.
#include "persist_internal.h"

#include "ai.h"
#include "clock.h"
#include "combat.h"
#include "containers.h"
#include "crashlog.h"
#include "dialogaudio.h"
#include "doors.h"
#include "downed.h"
#include "hero.h"
#include "journal.h"
#include "log.h"
#include "npcs.h"
#include "personal_store.h"
#include "remote.h"
#include "steam.h"
#include "story.h"
#include "trade.h"
#include "world.h"
#include "xpshare.h"

#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace g3t::persist::detail {

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

bool WriteAtomic(std::string const& path, Bytes const& bytes)
{
    if (saving::store::Atomic(path, bytes, nullptr, &lastIoFailure)) {
        Logf("persist: file operation=atomic-publish path=%s bytes=%zu ok=1", path.c_str(), bytes.size());
        return true;
    }
    LogIo(lastIoFailure);
    return false;
}

uint64_t GameModuleBytes()
{
    char path[MAX_PATH]{};
    WIN32_FILE_ATTRIBUTE_DATA data{};
    HMODULE game = GetModuleHandleA("Game.dll");
    if (!game || !GetModuleFileNameA(game, path, MAX_PATH) || !GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
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

std::string CharacterPath()
{
    const std::string folder = Folder();
    return folder.empty() ? std::string() : folder + "g3t-character-" + std::to_string(steam::PlayerId()) + "-local.g3thero";
}

bool ValidateCharacter(Bytes const& bytes, saving::Key const& key)
{
    saving::Key found;
    return saving::store::HeroKey(bytes, found) && saving::Exact(found, key) &&
           hero::ValidateRecordMemory(bytes.data(), bytes.size(), key);
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

bool CapturePersonalWithFallback(uint64_t account, Bytes& state)
{
    saving::personal::Snapshot decoded;
    if (containers::CapturePersonal(state) && saving::personal::Decode(state, decoded)) return true;
    state.clear();
    if (net::IsHost()) {
        auto const* previous = saving::store::Find(current, saving::store::Personal, account);
        if (previous && saving::personal::Decode(previous->bytes, decoded)) {
            state = previous->bytes;
            Logf("WARN persist: personal container capture degraded account=%llu; retaining previous snapshot bytes=%zu",
                 account, state.size());
            return true;
        }
    }
    if (!saving::personal::Encode({}, state)) return false;
    Logf("WARN persist: personal container capture degraded account=%llu; using empty personal snapshot", account);
    return true;
}

bool CapturePlayer(saving::Key const& key, Bytes& bytes, char const*& reason)
{
    Bytes hero, state;
    if (!CaptureCharacter(key, hero)) { reason = "character-write"; return false; }
    if (!CapturePersonalWithFallback(key.player, state) || !saving::store::PackPlayer(hero, state, bytes)) {
        state.clear();
        if (!saving::personal::Encode({}, state) || !saving::store::PackPlayer(hero, state, bytes)) {
            reason = "character-write";
            return false;
        }
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
        if (entry.second.kind != saving::personal::Kind::GroundTaken) return false;
    return true;
}

bool ApplyCharacter(Bytes const& bytes, saving::Key const& key)
{
    const bool ok = hero::LoadRecordFromMemory(bytes.data(), bytes.size(), key);
    Logf("persist: character restore account=%llu save=%s bytes=%zu ok=%d reason=%s",
         key.player, key.save.c_str(), bytes.size(), ok ? 1 : 0, hero::LastRefusal() ? hero::LastRefusal() : "-");
    if (ok) {
        ++recordsApplied;
        combat::OnWorldChange();
        xpshare::OnWorldChange();
    }
    return ok;
}

net::SaveNote Note(uint8_t op, Save const& save, bool ok)
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
            if (!removed) LogIo(error);
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

bool AllReady() { return operation.AllReady(); }

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
    DWORD failure = nativeOk ? 0 : GetLastError();
    const bool gone = !Exists(Path(slot, 0)) && !Exists(Path(slot, 1));
    if (gone && (!Exists(path) || DeleteFileA(path.c_str()))) {
        Logf("persist: host slot deleted slot=%s native_ok=%d", slot.c_str(), nativeOk ? 1 : 0);
        return true;
    }
    std::string errorPath = path;
    if (gone) {
        failure = GetLastError();
    } else if (!failure || failure == ERROR_FILE_NOT_FOUND) {
        for (int f = 0; f < 2; ++f) {
            const std::string slotFile = Path(slot, f);
            if (!Exists(slotFile)) continue;
            errorPath = slotFile;
            HANDLE h = CreateFileA(slotFile.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) { failure = GetLastError(); break; }
            CloseHandle(h);
        }
    }
    if (recoverable && !Materialize(slot, intact)) Logf("persist: delete recovery failed slot=%s", slot.c_str());
    Logf("persist: slot deletion failed slot=%s native_ok=%d error=%lu; container retained recoverable=%d",
         slot.c_str(), nativeOk ? 1 : 0, failure, recoverable ? 1 : 0);
    SaveFailure(saving::store::IoReason({ "delete", errorPath, {}, failure }, "save-delete-failed"));
    return false;
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
        saving::Key embedded;
        uint32_t heroVersion = 0;
        if (!saving::store::UnpackPlayer(bytes, hero, state) || !ValidateCharacter(hero, key) ||
            !saving::store::HeroKey(hero, embedded, &heroVersion)) {
            CancelSave("character-record"); return;
        }
        saving::store::Set(savingManifest, {saving::store::Character, heroVersion, key.player, std::move(hero)});
        saving::store::Set(savingManifest, {saving::store::Personal, 1, key.player, std::move(state)});
        operation.Accept(from, net::PeerGeneration(from), key.player, note.epoch);
        Logf("persist: character received save=%s account=%llu bytes=%zu all_ready=%d",
             note.save, key.player, bytes.size(), AllReady() ? 1 : 0);
        return;
    }
    if (note.epoch != hostWorld.epoch || key.player != steam::PlayerId()) return;
    requestCharacterAgain = false;
    if (synced == hostWorld.epoch && ownCharacterReady) {
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
        if (!containers::RestorePersonal({}))
            Log("WARN persist: clearing personal containers degraded while awaiting fresh hero");
        if (clientRejoin.CanUseFresh(gCSession::GetInstance().IsGameRunning(), hostWorld.kind != net::KindMenu) && templateHeroFresh) {
            if (!templateCapture.Holding(GetTickCount())) {
                needsNewHero = false; ownCharacterReady = true;
                clientRejoin.Left();
                synced = hostWorld.epoch; story::PrepareAdmission(); doors::PrepareAdmission(); Evaluate();
            }
        } else Notice(i18n::SaveCharacterMissing, true);
        Logf("persist: no saved character account=%llu epoch=%08x; fresh New Game required", key.player, note.epoch);
    } else {
        needsNewHero = false;
        saving::Key embedded;
        if (!saving::store::UnpackPlayer(bytes, receivedRecord, receivedPersonal) ||
            !saving::store::HeroKey(receivedRecord, embedded) || !saving::Exact(embedded, key)) {
            restoreFailed = true; receivedRecord.clear(); ClientRestoreFailure("character-record");
        }
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
    journal::Read(file); rewind(file);
    if (!npcs::ReadCompanions(file)) { fclose(file); DeleteFileA(path.c_str()); return false; }
    std::vector<uint64_t> accounts;
    for (auto const& entry : current.blocks)
        if (entry.type == saving::store::Character) accounts.push_back(entry.account);
    npcs::CompanionAccounts(accounts);
    rewind(file);
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
        char const* decodeReason = nullptr;
        if (!saving::store::Decode(bytes, save, &decodeReason)) {
            Logf("persist: load refused slot=%s reason=container-format detail=%s bytes=%zu",
                 slot.c_str(), decodeReason ? decodeReason : "-", bytes.size());
            Notice(i18n::SaveDamaged, true); return false;
        }
        auto const* own = saving::store::Find(save, saving::store::Character, steam::PlayerId());
        if (!own || !ValidateCharacter(own->bytes, {save.key.world, save.key.save, steam::PlayerId()})) {
            Logf("persist: load refused slot=%s save=%s reason=character-record", slot.c_str(), save.key.save.c_str());
            Notice(i18n::SaveDamaged, true); return false;
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
    Logf("persist: load validated slot=%s managed=%d sidecar=%d schema=%u min_reader=%u save=%s blocks=%zu",
         slot.c_str(), managed ? 1 : 0, hasSidecar ? 1 : 0, save.schemaVersion, save.minReaderVersion,
         save.key.save.c_str(), save.blocks.size());
    CancelSave("canceled-load");
    lastLoad = slot; loading = slot; current = std::move(save); campaign = slot;
    localKind = net::KindMenu; ownCharacterReady = false; restoreFailed = false;
    NewEpoch();
    ++loads; newWorld = false;
    // Release every puppet, NPC mirror/lease, summon, and dual PVS prefetcher job while old entities are still alive.
    if (worldCleanup) worldCleanup();
    clock::OnWorldChange();
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

void HostNotice(i18n::Message message, uint8_t peer, hud::Kind kind)
{
    wchar_t name[80] = {}, text[300] = {};
    MultiByteToWideChar(CP_UTF8, 0, net::PeerName(peer), -1, name, 79);
    if (!*name) wcscpy_s(name, L"Player");
    _snwprintf_s(text, _TRUNCATE, i18n::Text(message), name);
    NoticeText(text, kind);
}
void NewEpoch()
{
    CancelSave("world");
    static const uint32_t nonce = ((GetTickCount() * 2654435761u) ^ (GetCurrentProcessId() << 16)) & 0xffff0000u;
    epoch = nonce | (++epochs & 0xffff);
    if (!epoch) epoch = ++epochs;
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        clients[i].ready = clients[i].recordSent = clients[i].cutover = clients[i].nextTry = 0;
        clients[i].waitingSince = GetTickCount();
        clients[i].told = false;
        clients[i].hasSavedPose = false;
        clients[i].savedPose = {};
        net::SetInWorld(i, false);
        if (!net::PeerJoined(i)) continue;
        net::WorldNote notice{};
        notice.op = net::WorldHost; notice.kind = net::KindMenu; notice.epoch = epoch;
        strncpy_s(notice.campaign, campaign.c_str(), _TRUNCATE);
        net::SendWorld(notice, i);
    }
    Logf("persist: host world epoch=%08x kind=%u campaign=%s", epoch, localKind, campaign.c_str());
}

void HostTick(uint32_t now)
{
    if (!epoch) NewEpoch();
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        auto& client = clients[i];
        if (!net::PeerJoined(i)) { client = {}; continue; }
        const uint32_t generation = net::PeerGeneration(i);
        if (client.generation != generation) {
            client = {};
            client.generation = generation;
            client.waitingSince = now ? now : 1;
            Logf("admission: client peer=%u account=%llu connected, waiting to enter the host's world epoch=%08x kind=%u", i,
                 net::PeerAccount(i), epoch, ownCharacterReady && loading.empty() && !restoreFailed ? localKind : net::KindMenu);
            HostNotice(i18n::HostClientWaiting, i, hud::Info);
        }
        if (client.ready == epoch) continue;
        if (!client.told && now - client.waitingSince >= 20000) {
            client.told = true;
            Logf("admission: client peer=%u waiting %u s, not in the host's world: epoch=%08x host_kind=%u record_sent=%d cutover=%d in_world=%d",
                 i, (now - client.waitingSince) / 1000, epoch, localKind, client.recordSent == epoch ? 1 : 0,
                 client.cutover == epoch ? 1 : 0, net::InWorld(i) ? 1 : 0);
        }
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
        saving::Key recordKey;
        saving::RecordPose recordPose;
        const bool hasSavedPose = record && saving::store::HeroPose(record->bytes, recordKey, recordPose) &&
                                  saving::ValidRecordPose(recordPose);
        if (record) {
            auto const* personal = saving::store::Find(current, saving::store::Personal, account);
            saving::personal::Snapshot snapshot;
            if (personal && saving::personal::Decode(personal->bytes, snapshot)) state = personal->bytes;
            else if (!saving::personal::Encode({}, state)) { SaveFailure("personal-record"); return; }
            if (!saving::store::PackPlayer(record->bytes, state, envelope)) { SaveFailure("character-record"); return; }
        }
        if (SendRecord(i, net::CharacterRestore, {current.key.world, current.key.save, account},
                       envelope)) {
            client.recordSent = epoch;
            client.hasSavedPose = hasSavedPose;
            client.savedPose = hasSavedPose ? recordPose : saving::RecordPose{};
        }
    }
}

} // namespace g3t::persist::detail

namespace g3t::persist {
void AppendPendingAdmissionAnchors(std::vector<std::array<float, 3>>& out)
{
    using namespace detail;
    if (!net::IsHost() || !epoch || !ownCharacterReady || !loading.empty() || restoreFailed) return;
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) {
        auto const& client = clients[i];
        if (!net::PeerJoined(i) || net::InWorld(i) || client.recordSent != epoch || !client.hasSavedPose) continue;
        out.push_back({client.savedPose.position[0], client.savedPose.position[1], client.savedPose.position[2]});
    }
}
} // namespace g3t::persist

namespace g3t::persist::SaveExtensions {
bool Register(char const* name, uint32_t type, uint32_t version, Capture capture, Restore restore)
{
    using namespace detail;
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
} // namespace g3t::persist::SaveExtensions
