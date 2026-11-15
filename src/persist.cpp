// SPDX-License-Identifier: GPL-3.0-or-later
#include "persist_internal.h"

#include "ai.h"
#include "clock.h"
#include "combat.h"
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
#include "npcs.h"
#include "partner.h"
#include "remote.h"
#include "steam.h"
#include "story.h"
#include "trade.h"
#include "world.h"
#include "xpshare.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace g3t::persist::detail {

namespace {
char const* CommonSaveSessionReason(gCSession& session, void*& outControlled)
{
    outControlled = nullptr;
    if (!session.IsGameRunning() || !session.GetHero()) return "not-running";
    if (session.GetPlayer() != session.GetHero()) return "control";
    gCWorld* world = gCWorld::GetCurrentWorld();
    if (!world || !world->GetWorldEntity()) return "world";
    if (Folder().empty()) return "folder";
    // IsSaveAllowed/StopControlNPC in the verified CP image share this controlled-NPC slot.
    std::memcpy(&outControlled, reinterpret_cast<unsigned char const*>(&session) + 0x28, sizeof(outControlled));
    return nullptr;
}

void TickClientSaveRound(uint32_t now)
{
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
}
} // namespace

char const* NativeSaveReason()
{
    if (!eCApplication::IsInitialised()) return "not-running";
    if (nativeLoadDepth || nativeSaveDepth || !loading.empty()) return "loading";
    gCSession& session = gCSession::GetInstance();
    if (downed::Held() || downed::NativeDown()) return "downed";
    void* controlled = nullptr;
    if (char const* reason = CommonSaveSessionReason(session, controlled)) return reason;
    gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
    if (!infos) return "world";
    Entity self(session.GetHero());
    char const* reason = saving::NativeSaveRefusal(infos->GetIsRunning() != GEFalse, controlled != nullptr,
                                                  self.IsDead(), self.NPC.DefeatedByPlayer != GEFalse);
    if (reason) return reason;
    return session.IsSaveAllowed() ? nullptr : "native-save";
}
char const* ClientSaveReason()
{
    if (!eCApplication::IsInitialised()) return "not-running";
    if (nativeLoadDepth || nativeSaveDepth || !loading.empty()) return "loading";
    gCSession& session = gCSession::GetInstance();
    void* controlled = nullptr;
    if (char const* reason = CommonSaveSessionReason(session, controlled)) return reason;
    if (controlled != nullptr) return "control";
    Entity self(session.GetHero());
    if (self.IsDead() && !downed::Held()) return "dead";
    return nullptr;
}
char const* SaveReadiness()
{
    if (!net::IsHost() && clientRejoin.pending) return "peer-loading";
    if (char const* reason = net::IsHost() ? NativeSaveReason() : ClientSaveReason()) return reason;
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

void Evaluate()
{
    if (net::IsHost() || !net::Joined() || !hostWorld.epoch || hostWorld.kind == net::KindMenu) return;
    if (seenWelcome != net::WelcomeSerial()) { net::SetInWorld(net::LocalId(), false); return; }
    const bool ready = !nativeLoadDepth && !clientRejoin.pending && loading.empty() && !newWorld && ownCharacterReady &&
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
                clients[from].hasSavedPose = false;
                net::SetInWorld(from, true); // One-shot NPC/motion replays are now behind the client's applied cutover.
                if (first) {
                    Logf("persist: admission confirmed peer=%u account=%llu epoch=%08x baselines=1", from, net::PeerAccount(from), epoch);
                    Logf("admission: client peer=%u account=%llu in the host's world epoch=%08x kind=%u after_ms=%u", from,
                         net::PeerAccount(from), epoch, localKind, GetTickCount() - clients[from].waitingSince);
                    HostNotice(i18n::HostClientInWorld, from, hud::Good);
                }
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
        } else if (note.op == net::WorldWaiting || note.op == net::WorldOther) {
            net::SetInWorld(from, false);
            if (note.op == net::WorldOther) {
                clients[from].ready = clients[from].recordSent = clients[from].cutover = 0;
                clients[from].nextTry = 0;
                clients[from].hasSavedPose = false;
            }
        }
        return;
    }
    if (note.op == net::WorldReady) {
        if (seenWelcome != net::WelcomeSerial() || note.epoch != hostWorld.epoch || note.kind == net::KindMenu || !ownCharacterReady ||
            synced != hostWorld.epoch || !story::AdmissionReady() || !doors::AdmissionReady()) return;
        hostAdmissionAck = true;
        Evaluate();
        return;
    }
    if (note.op != net::WorldHost) return;
    if (note.epoch != hostWorld.epoch) {
        const bool replacing = rejoin::ReplacesWorld(hostWorld.epoch, note.epoch,
            hostWorld.kind != net::KindMenu, gCSession::GetInstance().GetPlayer() != nullptr, clientRejoin.pending);
        if (replacing) {
            Logf("persist: client enter state=queued reason=host-world-changed epoch=%08x->%08x", hostWorld.epoch, note.epoch);
            QueueClientReturn();
            // Autoload: the entry still needs this epoch's countdown (and the config/cancel decision); otherwise the player enters manually.
            Notice(AutoloadAvailable() ? i18n::ClientHostWorldChanged : i18n::ClientChooseNewGame, true);
        }
        entryApproved = autoloadAwaiting = false; // a newer epoch restarts the countdown of an automatic entry not yet started
        templateFailures = 0; templateFailCode.clear(); templateFailError = 0;
        if (captured.valid || requestPending) Notice(i18n::ClientSaveCanceledByWorld, true);
        captured = {}; requestPending = false; receivedRecord.clear(); receivedPersonal.clear(); receivedNote = {};
        needsNewHero = false; restoreFailed = false; synced = 0; ownCharacterReady = false; hostAdmissionAck = false;
        nextAdmissionTry = 0;
        net::SetInWorld(net::LocalId(), false);
        // Live native objects are retired at the next outer module boundary, never in ENet dispatch.
        if (!clientRejoin.pending) {
            story::OnWorldChange(); doors::OnWorldChange(); clock::OnWorldChange();
            combat::OnWorldChange(); xpshare::OnWorldChange();
        }
    }
    hostWorld = note;
    Evaluate();
}
void CALLBACK SaveNow(HWND, UINT, UINT_PTR, DWORD)
{
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
        saving::Key heroKey;
        uint32_t heroVersion = 0;
        if (!saving::store::HeroKey(hero, heroKey, &heroVersion)) { CancelSave("character-write"); return; }
        saving::store::Set(savingManifest, {saving::store::Character, heroVersion, steam::PlayerId(), std::move(hero)});
        if (!CapturePersonalWithFallback(steam::PlayerId(), state)) { CancelSave("personal-record"); return; }
        saving::store::Set(savingManifest, {saving::store::Personal, 1, steam::PlayerId(), std::move(state)});
        if (!CaptureJournal(state)) { CancelSave("journal-write"); return; }
        saving::store::Set(savingManifest, {saving::store::Journal, 1, 0, std::move(state)});
        if (!CaptureExtensions(savingManifest)) { CancelSave("extension"); return; }
        // The thumbnail was fixed at request time (PrepareSaveThumbnail); the menu is still drawn now.
        // Save host-native NPCs as they stand; do not import or revoke live client authority.
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
    Logf("persist: host container published schema=%u min_reader=%u save=%s slot=%s bytes=%zu records=%zu menu_ok=%d network_world_bytes=0",
         current.schemaVersion, current.minReaderVersion, current.key.save.c_str(), requestedSlot.c_str(),
         encoded.size(), current.blocks.size(), 1);
    FinishSave(true, "durable host container");
}
void* RetainedImage(void* manager)
{
    void* image = nullptr;
    std::memcpy(&image, static_cast<unsigned char*>(manager) + saving::thumb::ImageOffset, sizeof(image));
    return image;
}
void CaptureSceneImage(void* manager)
{
    void* capture = uiEntries[4];
    __asm {
        push esi
        mov esi, manager
        call capture
        pop esi
    }
}
// Native captures the thumbnail when the menu opens or at the quicksave message, never when saving. The host-managed
// save runs later, while the save page is still drawn, so choose the picture at the request and never recapture.
void PrepareSaveThumbnail()
{
    void* manager = FileManager(false);
    if (!manager) return;
    auto* gui = gCSession::GetInstance().GetGUIManager();
    const HWND window = eCApplication::GetInstance().GetHandle();
    saving::thumb::View view;
    view.menuOpen = gui && gui->IsMenuOpen();
    view.pageOpen = gui && gui->IsAnyPageOpen();
    view.minimized = window && IsIconic(window);
    view.retained = RetainedImage(manager) != nullptr;
    view.retainedCurrent = view.retained && thumbnail.Current(epoch);
    const auto choice = saving::thumb::Choose(view);
    if (choice != saving::thumb::Choice::KeepRetained) CaptureSceneImage(manager);
    if (choice == saving::thumb::Choice::CaptureScene || (view.menuOpen && view.retained)) thumbnail.Stamp(epoch);
    Logf("persist: save thumbnail source=%s menu=%d page=%d minimized=%d retained=%d current=%d image=%d",
         saving::thumb::Name(choice), view.menuOpen ? 1 : 0, view.pageOpen ? 1 : 0, view.minimized ? 1 : 0,
         view.retained ? 1 : 0, view.retainedCurrent ? 1 : 0, RetainedImage(manager) ? 1 : 0);
}
// With the menu open, native captured the image before showing it: remember that it belongs to this world.
void ObserveThumbnail()
{
    auto& game = gCSession::GetInstance();
    auto* gui = game.GetGUIManager();
    if (net::IsHost() && game.IsGameRunning() && gui && gui->IsMenuOpen()) thumbnail.Stamp(epoch);
}
// Native save of the running world into the template slot. The world half is still being written when it returns.
bool TemplateStart(rejoin::tmpl::Origin origin, TemplateJob job)
{
    namespace tpl = rejoin::tmpl;
    void* manager = FileManager(false);
    const auto paths = TemplatePaths();
    const auto identity = TemplateIdentity();
    const uint32_t now = GetTickCount();
    templateJob = job;
    templateIoError = 0;
    templateNextRecover = now; // a tick-relative reference: the signed comparison is only valid against a recent value
    Logf("template: save start job=%s origin=%s attempt=%u slot=%s folder=%s game=%llu protocol=%u epoch=%08x hero=%p",
         TemplateJobName(job), tpl::OriginName(origin), ++templateAttempts, tpl::Slot, Folder().c_str(),
         static_cast<unsigned long long>(identity.game), static_cast<unsigned>(identity.protocol), hostWorld.epoch,
         gCSession::GetInstance().GetHero());
    if (!manager || !originalSave || !uiEntries[4] || !identity.game || !tpl::OwnSlot(tpl::Slot) || !paths.Usable()) {
        Logf("template: refused reason=native-hooks manager=%p save=%p capture=%p game=%llu folder=%d", manager,
             reinterpret_cast<void*>(originalSave), uiEntries[4], static_cast<unsigned long long>(identity.game),
             paths.Usable() ? 1 : 0);
        templatePublish.Begin(origin, now);
        templatePublish.Abort(paths.Usable() ? tpl::Fail::NativeRefused : tpl::Fail::Folder);
        TemplateEnded(now);
        return false;
    }
    // The engine's final rename never replaces: every earlier file of the slot, staging files included, must be gone first.
    tpl::store::Error error;
    if (!tpl::store::RemoveOwn(paths, error, &TemplateTrace)) {
        LogIo(error);
        templateIoError = error.code;
        templatePublish.Begin(origin, now);
        templatePublish.Abort(tpl::store::FailFor(error, tpl::Fail::Cleanup));
        TemplateEnded(now);
        return false;
    }
    TemplateInvalidate();
    remote::DespawnAll("client rejoin template"); // puppets are not part of the template world (as in the host save)
    {
        void* capture = uiEntries[4];
        __asm {
            push esi
            mov esi, manager
            call capture
            pop esi
        }
    }
    const bCString name(tpl::Slot);
    lowerSaveCalls = 0; lowerSaveGood = true;
    templatePublish.Begin(origin, now);
    ++nativeSaveDepth;
    const bool nativeOk = originalSave(manager, &name);
    --nativeSaveDepth;
    const uint32_t returned = GetTickCount();
    templatePublish.NativeReturned(nativeOk, lowerSaveCalls, lowerSaveGood, returned);
    Logf("template: native HUD.SaveGame slot=%s ok=%d lower_calls=%u lower_ok=%d ms=%u", tpl::Slot, nativeOk ? 1 : 0,
         lowerSaveCalls, lowerSaveGood ? 1 : 0, returned - now);
    templateSettleShown.clear();
    if (!templatePublish.Active()) { TemplateEnded(returned); return false; }
    return true;
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
    // Retain absent accounts and unknown extensions, except the retired client-NPC overlay.
    for (auto block : current.blocks) {
        if (block.type == LegacyNpcLeaseExtension) continue;
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
    PrepareSaveThumbnail();
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
    if (!*reason && !refusal.empty()) {  // a new invite is a new attempt: the old refusal no longer applies
        refusal.clear();
        shown.clear();
    }
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
    auto* manager = gCSession::GetInstance().GetGUIManager();
    const bool inMenu = manager && manager->IsMenuOpen();
    const uint32_t trying = net::ConnectingFor();
    if (!config.host && rejoinHooks && !clientRejoin.pending) {
        if (hud::ClientNewGameEnabled(rejoin::MenuEnabled(true, rejoinHooks, nativeLoadDepth || nativeSaveDepth))) menuButtonSeen = now ? now : 1;
    }
    wchar_t autoloadBuffer[700] = {};
    wchar_t const* autoloadText = nullptr;
    // Native hooks off (either role): said loudly in the menu line and on the HUD, never as a plain "connected".
    static bool unsupportedLogged = false;
    static uint32_t unsupportedAt = 0;
    const bool unsupported = HooksUnsupported();
    if (unsupported) {
        if (!unsupportedLogged) {
            unsupportedLogged = true;
            Logf("admission: unsupported install role=%s cause=%s pup_digest=%s; host-managed saves OFF, a client can never be admitted",
                 config.host ? "host" : "client", hookFailure.empty() ? "hooks" : hookFailure.c_str(),
                 pupDigestHex.empty() ? "-" : pupDigestHex.c_str());
        }
        if (running && (!unsupportedAt || now - unsupportedAt >= 30000)) {
            unsupportedAt = now ? now : 1;
            wchar_t text[700] = {};
            hud::Message(hud::Bad, UnsupportedText(text, 700));
        }
    }
    if (!running || inMenu) {
        if (unsupported) {
            if (hud::MenuLine(UnsupportedText(autoloadBuffer, 700))) menuSet = true;
        } else if (!refusal.empty()) {
            if (hud::MenuLine(refusal.c_str())) menuSet = true;
        } else if (!config.host && !(clientRejoin.pending && (entryManual || entryApproved)) &&
                   (autoloadText = AutoloadMenuText(autoloadBuffer, 700)) != nullptr) {
            if (hud::MenuLine(autoloadText)) menuSet = true;
        } else if (!config.host && clientRejoin.pending) {
            const auto hint = rejoinWait == rejoin::Wait::Save ? i18n::ClientRejoinWaitSave :
                              rejoinWait == rejoin::Wait::Interaction ? i18n::ClientRejoinWaitTrade : i18n::ClientReturningToMenu;
            if (hud::MenuLine(i18n::Text(hint))) menuSet = true;
        } else if (!config.host && (clientRejoin.failed || restoreFailed)) {
            if (hud::MenuLine(i18n::Text(i18n::ClientRejoinFailed))) menuSet = true;
        } else if (joined && hostWorld.kind == net::KindMenu) {
            if (hud::MenuLine(i18n::Text(i18n::ClientHostLoading))) menuSet = true;
        } else if (joined && needsNewHero) {
            if (hud::MenuLine(i18n::Text(i18n::SaveCharacterMissing))) menuSet = true;
        } else if (joined && hostWorld.epoch && !ownCharacterReady) {
            if (hud::MenuLine(i18n::Text(i18n::ClientChooseNewGame))) menuSet = true;
        } else if (joined && ownCharacterReady) {
            // Honest about the world: "connected" alone never says the player is in the host's world.
            wchar_t host[80], head[240], text[700];
            HostNameWide(host);
            _snwprintf_s(head, _TRUNCATE, i18n::Text(net::Connected() ? i18n::AdmissionInWorld : i18n::AdmissionJoining), host);
            _snwprintf_s(text, _TRUNCATE, L"%s %s", head, i18n::Text(i18n::ClientMenuNewGame));
            if (hud::MenuLine(text)) menuSet = true;
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

} // namespace g3t::persist::detail

namespace g3t::persist {

using namespace detail;

bool PrepareShutdown()
{
    if (nativeLoadDepth || nativeSaveDepth) {
        Logf("persist: shutdown still inside native operation load=%u save=%u; fast exit disabled",
             nativeLoadDepth, nativeSaveDepth);
        return false;
    }
    CancelSave("shutdown");
    if (templatePublish.Active()) {
        // Never leave a half template behind: the engine's writer may still hold a staging file, the next start cleans up.
        templatePublish.Abort(rejoin::tmpl::Fail::Abandoned);
        templateJob = TemplateJob::None;
        RemoveTemplate("shutdown");
    }
    StopSaveTimer();
    CleanupStaging();
    RetryCleanup(GetTickCount() + 3001);
    captured = {};
    requestPending = false;
    loading.clear();
    receivedRecord.clear();
    receivedPersonal.clear();
    Log("persist: shutdown cooperative transfers cancelled; committed saves unchanged");
    return true;
}
void Tick()
{
    if (nativeLoadDepth || nativeSaveDepth) return;
    const uint32_t now = GetTickCount();
    RetryCleanup(now);
    RetryReplies(now); StatusHints();
    ObserveThumbnail();
    auto& game = gCSession::GetInstance();
    const bool running = game.GetHero() && game.IsGameRunning();
    // The one-shot fresh-hero authorization waits out the template save: consuming newWorld inside the hold lost it.
    const bool templateHold = templateCapture.Holding(now);
    if (newWorld && running && !templateHold) {
        newWorld = false; localKind = net::KindNew;
        resetLocalWorld = false;
        // Only a freshly initialized local hero is permitted when the host has no account record.
        if (net::IsHost()) {
            ownCharacterReady = true; current.key = {RandomId(), RandomId(), steam::PlayerId()}; NewEpoch();
        }
    }
    if (net::IsHost()) {
        PollSave(now);
        if (!loading.empty() && running && static_cast<int32_t>(now - restoreAt) >= 0) {
            if (!PersonalReadyForRestore(receivedPersonal)) {
                if (now - restoreAt <= 3000) { HostTick(now); return; }
                Log("WARN persist: personal hooks unavailable during host load; continuing without personal container overlay");
                SaveFailure("personal-hooks");
                receivedPersonal.clear();
            }
            const bool ownOk = receivedRecord.empty() || ApplyCharacter(receivedRecord, pendingKey);
            if (!receivedPersonal.empty() && !containers::RestorePersonal(receivedPersonal))
                Log("WARN persist: host personal container restore degraded; character and world remain active");
            const char* refusal = !ownOk ? "character-record" :
                                  !RestoreJournal() ? "journal-write" :
                                  !RestoreExtensions() ? "extension" : nullptr;
            if (refusal) {
                receivedRecord.clear(); receivedPersonal.clear();
                restoreFailed = true; loading.clear();
                Logf("persist: host load failed slot=%s save=%s reason=%s", lastLoad.c_str(), current.key.save.c_str(), refusal);
                Notice(i18n::SaveDamaged, true);
                return;
            }
            receivedRecord.clear(); receivedPersonal.clear(); loading.clear(); ownCharacterReady = true; localKind = net::KindSaved;
            for (uint8_t i = 2; i < net::MaxPlayers; ++i) clients[i].nextTry = 0;
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
    TickClientSaveRound(now);
    if (clientRejoin.pending) return;
    if (requestCharacterAgain && hostWorld.epoch && hostWorld.kind != net::KindMenu) {
        net::WorldNote answer{};
        answer.op = net::WorldOther; answer.kind = localKind; answer.epoch = hostWorld.epoch;
        if (net::SendWorld(answer, 1)) requestCharacterAgain = false;
    }
    if (needsNewHero && templateHeroFresh && clientRejoin.CanUseFresh(running, hostWorld.kind != net::KindMenu) &&
        !restoreFailed && !resetLocalWorld && !templateHold &&
        receivedWelcome == net::WelcomeSerial() && receivedNote.epoch == hostWorld.epoch) {
        needsNewHero = false; ownCharacterReady = true; synced = hostWorld.epoch;
        clientRejoin.Left();
        story::PrepareAdmission(); doors::PrepareAdmission(); Evaluate();
    }
    if (!receivedRecord.empty() && !restoreFailed && !resetLocalWorld && clientRejoin.CanUseFresh(running, hostWorld.kind != net::KindMenu) &&
        !templateCapture.Holding(now) && receivedWelcome == net::WelcomeSerial() &&
        receivedNote.epoch == hostWorld.epoch && static_cast<int32_t>(now - restoreAt) >= 0) {
        if (!PersonalReadyForRestore(receivedPersonal)) {
            if (now - restoreAt <= 3000) return;
            Log("WARN persist: personal hooks unavailable during client restore; admitting character without personal overlay");
            SaveFailure("personal-hooks");
            receivedPersonal.clear();
        }
        if (!ApplyCharacter(receivedRecord, {receivedNote.world, receivedNote.save, steam::PlayerId()})) {
            restoreFailed = true; ClientRestoreFailure("character-record"); return;
        }
        if (!containers::RestorePersonal(receivedPersonal))
            Log("WARN persist: client personal container restore degraded; admitting restored character");
        receivedRecord.clear(); receivedPersonal.clear(); ownCharacterReady = true; synced = hostWorld.epoch;
        clientRejoin.Left();
        story::PrepareAdmission(); doors::PrepareAdmission(); Evaluate();
    }
    if (ownCharacterReady && !net::InWorld(net::LocalId()) && synced == hostWorld.epoch &&
        story::AdmissionReady() && doors::AdmissionReady()) Evaluate();
}
void Install()
{
    if (installed) return;
    net::SetSaveHandler(&OnSave);
    net::SetCharacterHandler(&OnCharacter);
    net::SetWorldHandler(&OnWorld);
    unsigned char* resolved[5]{};
    if (!ResolveNative(resolved)) {
        if (hookFailure.empty()) hookFailure = "Game.dll layout";
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
    if (!net::IsHost() && rejoin::native::Matches(nativeImage, nativeImageSize)) {
        originalNewGameCommand = reinterpret_cast<NewGameCommandFn>(ai::JumpTo(nativeImage + rejoin::native::Command.hint,
            reinterpret_cast<void*>(&NewGameCommandDetour), rejoin::native::CommandStolen));
        originalNewGame = reinterpret_cast<NewGameFn>(ai::JumpTo(nativeImage + rejoin::native::Loader.hint,
            reinterpret_cast<void*>(&NewGameDetour), rejoin::native::LoaderStolen));
        rejoinHooks = originalNewGameCommand && originalNewGame && pairedEnabled;
        // Called, never hooked: the confirmed-click branch that Matches() verified (bytes, call edge and file-manager operand).
        if (rejoinHooks) confirmedStart = reinterpret_cast<ConfirmedStartFn>(nativeImage + rejoin::native::Confirmed.hint);
        Logf("persist: client rejoin native hooks ready=%d; menu player gate and process-exit branch guarded; autoload=%s", rejoinHooks ? 1 : 0,
             rejoinHooks && GetConfig().clientAutoload ? "on" : "off");
    } else if (!net::IsHost()) {
        clientRejoin.failed = true;
        if (hookFailure.empty()) hookFailure = "menu/loader layout";
        Log("persist: client rejoin unavailable; native menu/loader ABI mismatch");
    }
    installed = true;
    if (!pairedEnabled) {
        if (hookFailure.empty()) hookFailure = "hook install";
        Logf("persist: save/load hook installation failed SaveGameFn=%d LoadGameFn=%d WorldSaveGameFn=%d WorldLoadGameFn=%d; partial hooks pass through, host-managed saves off",
             originalSave != nullptr, originalLoad != nullptr, originalWorldSave != nullptr, loadCallInstalled ? 1 : 0);
        return;
    }
    Logf("persist: save and load hooked SaveGameFn=%p LoadGameFn=%p WorldSaveGameFn=%p WorldLoadGameFn=%p FileManagerGetter=%p protocol=%u",
         resolved[0], resolved[1], resolved[2], resolved[3], resolved[4], saving::Protocol);
    Logf("persist: host-managed save UI hooked GUI.HandleMessage=%p Menu.NewSaveSlotCall=%p Session.IsSaveAllowed=%p HUD.CaptureSaveImage=%p",
         uiEntries[0], uiEntries[2] + 6, uiEntries[3], uiEntries[4]);
}
} // namespace g3t::persist
