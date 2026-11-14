// SPDX-License-Identifier: GPL-3.0-or-later
// Client rejoin template lifecycle, autoload controller, and safe outer-boundary world transition.
#include "persist_internal.h"

#include "ai.h"
#include "clock.h"
#include "config.h"
#include "crashlog.h"
#include "log.h"
#include "partner.h"
#include "steam.h"
#include "trade.h"

#include <cstdio>

namespace g3t::persist::detail {
namespace {

rejoin::tmpl::Report templateReport;
uint32_t templateCheckedAt = 0;
uint64_t templateVerified = 0;
std::string templateImpossibleCode;

char const* SettleText(rejoin::tmpl::Facts const& f, char* buffer, size_t size)
{
    const auto show = [](rejoin::tmpl::File const& file, char* out, size_t n) {
        if (!file.exists) _snprintf_s(out, n, _TRUNCATE, "missing(%lu)", static_cast<unsigned long>(file.error));
        else _snprintf_s(out, n, _TRUNCATE, "%llu%s", static_cast<unsigned long long>(file.size),
                         file.locked ? "+locked" : "");
    };
    char header[40], world[40], headerTmp[40], worldTmp[40];
    show(f.header, header, sizeof(header)); show(f.world, world, sizeof(world));
    show(f.headerTmp, headerTmp, sizeof(headerTmp)); show(f.worldTmp, worldTmp, sizeof(worldTmp));
    _snprintf_s(buffer, size, _TRUNCATE, "header=%s world=%s header_tmp=%s world_tmp=%s", header, world, headerTmp, worldTmp);
    return buffer;
}

char const* AdmissionCause(autoload::Status status)
{
    using S = autoload::Status;
    switch (status) {
    case S::Unsupported: return hookFailure.empty() ? "hooks-off" : hookFailure.c_str();
    case S::Refused: return "refused";
    case S::HostLoading: return "host-has-no-world";
    case S::ManualOnly: return "autoload-off";
    case S::NoRoute: return "no-template";
    case S::Held: return "canceled";
    case S::GaveUp: return "entry-failed";
    case S::Stalled: return "admission-timeout";
    case S::Mismatch: return "not-host-world";
    case S::JoiningSlow: return "slow-admission";
    default: return "-";
    }
}

bool RecordKnown()
{
    return receivedNote.epoch == hostWorld.epoch && receivedWelcome == net::WelcomeSerial() &&
           (!receivedRecord.empty() || needsNewHero);
}

bool FirstEntryPossible()
{
    return confirmedStart && worldCleanup && !clientWorldEntered;
}

rejoin::tmpl::Plan EntryPlan(bool quiet)
{
    namespace tpl = rejoin::tmpl;
    auto& game = gCSession::GetInstance();
    const auto report = TemplateState();
    tpl::Situation s;
    s.health = report.health;
    s.origin = report.sealed ? report.seal.origin : tpl::Origin::NewGame;
    s.hooks = rejoinHooks && originalLoad && originalSave;
    s.worldEntered = clientWorldEntered;
    s.worldLoaded = game.GetHero() != nullptr || game.GetPlayer() != nullptr;
    s.worldRunning = s.worldLoaded && game.IsGameRunning() && !NativeSaveReason();
    s.quiet = quiet;
    s.recordKnown = RecordKnown() || recordWaitGaveUp;
    s.newAccount = needsNewHero && RecordKnown();
    s.jobActive = templatePublish.Active();
    s.failures = templateFailures;
    const auto plan = tpl::Decide(s);
    static const char* routes[] = {"template", "create", "first-entry", "wait", "impossible"};
    static std::string shown;
    char key[220];
    _snprintf_s(key, _TRUNCATE, "route=%s why=%s state=%s origin=%s entered=%d loaded=%d running=%d quiet=%d new_account=%d job=%d failures=%u",
                routes[static_cast<int>(plan.route)], tpl::BlockName(plan.why), tpl::HealthName(s.health), tpl::OriginName(s.origin),
                s.worldEntered ? 1 : 0, s.worldLoaded ? 1 : 0, s.worldRunning ? 1 : 0, s.quiet ? 1 : 0, s.newAccount ? 1 : 0,
                s.jobActive ? 1 : 0, s.failures);
    if (shown != key) {
        shown = key;
        Logf("template: %s", key);
    }
    templateImpossibleCode = plan.route == tpl::Route::Impossible
        ? (plan.why == tpl::Block::Failed && !templateFailCode.empty() ? templateFailCode : std::string(tpl::BlockCode(plan.why)))
        : std::string();
    return plan;
}

bool ClientQuietNow() { return rejoin::State{}.Waiting(ClientBoundary()) == rejoin::Wait::None; }

void TemplateImpossible(rejoin::tmpl::Plan plan)
{
    namespace tpl = rejoin::tmpl;
    const std::string code = plan.why == tpl::Block::Failed && !templateFailCode.empty() ? templateFailCode : tpl::BlockCode(plan.why);
    Logf("template: route=impossible why=%s code=%s error=%lu epoch=%08x", tpl::BlockName(plan.why), code.c_str(),
         static_cast<unsigned long>(templateFailError), hostWorld.epoch);
    static std::string shown;
    const std::string key = code + std::to_string(hostWorld.epoch);
    if (key == shown) return;
    shown = key;
    TemplateNotify(code.c_str(), templateFailError);
}

autoload::Snapshot AutoloadSnapshot(uint32_t now)
{
    auto& game = gCSession::GetInstance();
    auto* manager = game.GetGUIManager();
    autoload::Snapshot s;
    s.client = !net::IsHost();
    s.hooks = rejoinHooks && !ai::ShuttingDown();
    s.enabled = AutoloadAvailable();
    s.joined = net::Joined();
    s.refused = *net::RefusalReason() != 0;
    s.epoch = hostWorld.epoch;
    s.hostPlayable = hostWorld.kind != net::KindMenu;
    s.localWorld = game.GetPlayer() != nullptr || game.IsGameRunning();
    s.queued = clientRejoin.pending && !entryManual && !entryApproved;
    s.admitted = hostWorld.epoch && ownCharacterReady && synced == hostWorld.epoch && net::InWorld(net::LocalId());
    s.adoptable = s.localWorld && hostWorld.epoch && !restoreFailed && !clientRejoin.failed && !clientRejoin.pending &&
                  (clientRejoin.fresh || (ownCharacterReady && synced == hostWorld.epoch));
    s.busy = nativeLoadDepth || nativeSaveDepth || processDepth || menuCommandDepth || captured.valid || requestPending;
    s.recordKnown = RecordKnown();
    if (s.enabled && s.joined && s.epoch && s.hostPlayable && !s.admitted) {
        if (!routeCheckedAt || now - routeCheckedAt >= 1000) {
            routeCheckedAt = now ? now : 1;
            routeAvailable = HostWorldEntryAvailable();
        }
        s.routeAvailable = routeAvailable || (FirstEntryPossible() && !s.localWorld);
        const bool menuReady = manager && manager->IsMenuOpen() && menuButtonSeen && now - menuButtonSeen < 2500;
        s.entryReady = s.queued || (!s.localWorld && menuReady);
    }
    return s;
}

void CountdownText(wchar_t* buffer, size_t size, unsigned seconds)
{
    _snwprintf_s(buffer, size, _TRUNCATE,
                 i18n::Text(needsNewHero ? i18n::ClientAutoloadCountdownFresh : i18n::ClientAutoloadCountdown), seconds);
}

wchar_t const* AdmissionText(autoload::Status status, bool world, wchar_t* buffer, size_t size)
{
    using S = autoload::Status;
    wchar_t host[80];
    HostNameWide(host);
    const unsigned seconds = (autoloadRemainingMs + 999) / 1000;
    auto with = [&](i18n::Message message) {
        _snwprintf_s(buffer, size, _TRUNCATE, i18n::Text(message), host);
        return buffer;
    };
    switch (status) {
    case S::Unsupported: return UnsupportedText(buffer, size);
    case S::HostLoading:
    case S::WaitRecord: return i18n::Text(AutoloadAvailable() ? i18n::ClientAutoloadWaitHost : i18n::ClientHostLoading);
    case S::ManualOnly: return with(i18n::AdmissionManual);
    case S::Held: return world ? with(i18n::AdmissionHeld) : i18n::Text(i18n::ClientAutoloadCanceled);
    case S::GaveUp: return world ? with(i18n::AdmissionGaveUp) : i18n::Text(i18n::ClientAutoloadGaveUp);
    case S::NoRoute: {
        const char* code = templateImpossibleCode.empty() ? "template-unavailable" : templateImpossibleCode.c_str();
        _snwprintf_s(buffer, size, _TRUNCATE, i18n::Text(i18n::ClientTemplateUnavailable), i18n::SaveReason(code),
                     static_cast<unsigned long>(templateFailError));
        return buffer;
    }
    case S::Joining:
    case S::JoiningSlow:
    case S::Admitting: return with(i18n::AdmissionJoining);
    case S::Mismatch: return with(i18n::AdmissionMismatch);
    case S::Stalled: return with(i18n::AdmissionStalled);
    case S::Countdown:
        if (world && !clientRejoin.pending) {
            _snwprintf_s(buffer, size, _TRUNCATE, i18n::Text(i18n::AdmissionMismatchCountdown), seconds);
            return buffer;
        }
        CountdownText(buffer, size, seconds);
        return buffer;
    case S::Retry:
        _snwprintf_s(buffer, size, _TRUNCATE, i18n::Text(i18n::ClientAutoloadRetry), seconds);
        return buffer;
    default: return nullptr;
    }
}

void AdmissionObserve(autoload::Snapshot const& s, uint32_t now)
{
    static bool joined = false, world = false, admitted = false, fresh = false;
    static uint32_t seenEpoch = 0, waitingSince = 0;
    static uint8_t kind = 0xff;
    if (s.joined != joined) {
        joined = s.joined;
        if (joined)
            Logf("admission: connected host=%s hooks=%d autoload=%d protocol=%u welcome=%u", steam::HostName(), s.hooks ? 1 : 0,
                 s.enabled ? 1 : 0, saving::Protocol, net::WelcomeSerial());
        else
            Logf("admission: disconnected host=%s hooks=%d autoload=%d", steam::HostName(), s.hooks ? 1 : 0, s.enabled ? 1 : 0);
    }
    if (hostWorld.epoch != seenEpoch || hostWorld.kind != kind) {
        Logf("admission: host epoch=%08x kind=%u (was %08x/%u) client_world=%s admitted=%d", hostWorld.epoch, hostWorld.kind, seenEpoch,
             kind == 0xff ? 0u : kind, s.localWorld ? (s.adoptable ? "fresh" : "used") : "none", s.admitted ? 1 : 0);
        seenEpoch = hostWorld.epoch;
        kind = hostWorld.kind;
    }
    if (s.localWorld != world || (s.localWorld && s.adoptable != fresh)) {
        world = s.localWorld;
        fresh = s.adoptable;
        Logf("admission: client world=%s host_epoch=%08x admitted=%d queued=%d fresh=%d", world ? (fresh ? "fresh" : "used") : "none",
             hostWorld.epoch, s.admitted ? 1 : 0, s.queued ? 1 : 0, clientRejoin.fresh ? 1 : 0);
    }
    if (s.joined && s.epoch && s.hostPlayable && !s.admitted && !waitingSince) waitingSince = now ? now : 1;
    if (s.admitted != admitted) {
        admitted = s.admitted;
        if (admitted) {
            Logf("admission: admitted epoch=%08x after_ms=%u world=%s", hostWorld.epoch, waitingSince ? now - waitingSince : 0,
                 fresh ? "fresh" : "used");
            waitingSince = 0;
            if (s.client && s.localWorld) {
                wchar_t host[80], text[200];
                HostNameWide(host);
                _snwprintf_s(text, _TRUNCATE, i18n::Text(i18n::AdmissionInWorld), host);
                NoticeText(text, hud::Good);
            }
        } else Logf("admission: left host world epoch=%08x joined=%d", hostWorld.epoch, s.joined ? 1 : 0);
    }
}

void NoteAutoload(autoload::Decision const& decision, autoload::Snapshot const& snapshot, uint32_t now)
{
    using S = autoload::Status;
    static uint32_t remindedAt = 0;
    autoloadRemainingMs = decision.remainingMs;
    AdmissionObserve(snapshot, now);
    wchar_t text[700] = {};
    auto& game = gCSession::GetInstance();
    auto* manager = game.GetGUIManager();
    const bool hudVisible = game.IsGameRunning() && !(manager && manager->IsMenuOpen());
    if (decision.status == autoloadStatus) {
        const bool slow = decision.status == S::JoiningSlow || autoload::Blocked(decision.status);
        if (hudVisible && snapshot.localWorld && !snapshot.admitted && slow && decision.status != S::Unsupported &&
            now - remindedAt >= 20000 && AdmissionText(decision.status, true, text, 700)) {
            remindedAt = now;
            hud::Message(hud::Warning, text);
        }
        return;
    }
    autoloadStatus = decision.status;
    remindedAt = now;
    Logf("persist: client autoload status=%s epoch=%08x kind=%u entries=%u queued=%d record=%d", autoload::Name(decision.status),
         hostWorld.epoch, hostWorld.kind, autoloadState.Entries(), clientRejoin.pending ? 1 : 0, RecordKnown() ? 1 : 0);
    Logf("admission: phase=%s cause=%s host_epoch=%08x host_kind=%u world=%s admitted=%d queued=%d entries=%u route=%d autoload=%d hooks=%d",
         autoload::Name(decision.status), AdmissionCause(decision.status), hostWorld.epoch, hostWorld.kind,
         snapshot.localWorld ? (snapshot.adoptable ? "fresh" : "used") : "none", snapshot.admitted ? 1 : 0, snapshot.queued ? 1 : 0,
         autoloadState.Entries(), snapshot.routeAvailable ? 1 : 0, snapshot.enabled ? 1 : 0, snapshot.hooks ? 1 : 0);
    if (decision.status == S::Mismatch)
        Logf("admission: mismatch detected host_epoch=%08x kind=%u: this client's world is not the host's world and cannot be admitted by itself",
             hostWorld.epoch, hostWorld.kind);
    if (decision.status == S::Unsupported || !hudVisible) return;
    if (decision.status == S::Held && !snapshot.localWorld) return;
    if (decision.status == S::ManualOnly && snapshot.adoptable) return;
    if (!snapshot.localWorld && decision.status != S::Countdown) return;
    if (decision.status == S::Joining || decision.status == S::Admitting) return;
    if (AdmissionText(decision.status, snapshot.localWorld, text, 700)) {
        const bool bad = decision.status != S::Countdown && decision.status != S::HostLoading;
        hud::Message(bad ? hud::Warning : hud::Info, text);
    }
}

bool FirstEntryNow(uint32_t now)
{
    auto& game = gCSession::GetInstance();
    if (!FirstEntryPossible() || clientRejoin.pending || game.GetPlayer() || game.IsGameRunning() || !FileManager(true)) {
        Log("persist: client autoload first entry refused; native state not ready");
        autoloadState.Entered(false, now);
        return false;
    }
    Logf("persist: client autoload entering host world epoch=%08x via the first native New Game (no template yet)", hostWorld.epoch);
    hud::MenuLine(i18n::Text(i18n::ClientAutoloadEntering));
    clientRejoin.Left();
    confirmedStart();
    const bool ok = clientRejoin.fresh && !clientRejoin.failed;
    autoloadState.Entered(ok, GetTickCount());
    Logf("persist: client autoload first entry %s epoch=%08x entries=%u", ok ? "started" : "failed", hostWorld.epoch, autoloadState.Entries());
    Logf("admission: correction finished ok=%d via=first-new-game host_epoch=%08x entries=%u", ok ? 1 : 0, hostWorld.epoch,
         autoloadState.Entries());
    if (ok) Notice(i18n::ClientAutoloadEntered);
    return true;
}

// Outer module boundary (before FrameScope): never inside ENet dispatch, native load/save, nested frames or menu commands.
bool ProcessAutoload()
{
    if (nativeLoadDepth || nativeSaveDepth || processDepth || menuCommandDepth) return false;
    const uint32_t now = GetTickCount();
    if (autoloadAwaiting && !clientRejoin.pending) {
        autoloadAwaiting = entryApproved = false;
        const bool ok = clientRejoin.fresh && !clientRejoin.failed;
        autoloadState.Entered(ok, now);
        Logf("persist: client autoload entry %s epoch=%08x entries=%u", ok ? "started" : "failed", hostWorld.epoch, autoloadState.Entries());
        Logf("admission: correction finished ok=%d via=template host_epoch=%08x entries=%u", ok ? 1 : 0, hostWorld.epoch,
             autoloadState.Entries());
        if (ok) Notice(i18n::ClientAutoloadEntered);
    }
    const bool escDown = partner::Focused() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (escDown && !escWasDown && autoload::Cancelable(autoloadStatus)) AutoloadCancel("Esc", true);
    escWasDown = escDown;
    const auto snapshot = AutoloadSnapshot(now);
    const auto decision = autoloadState.Step(snapshot, now);
    NoteAutoload(decision, snapshot, now);
    if (decision.status == autoload::Status::NoRoute && clientRejoin.pending && !entryManual && !entryApproved &&
        !HostWorldEntryAvailable()) {
        entryApproved = true;
        return false;
    }
    if (decision.action != autoload::Action::Enter) return false;
    Logf("admission: correction started via=%s reason=%s host_epoch=%08x attempt=%u", HostWorldEntryAvailable() ? "template" : "first-new-game",
         snapshot.queued ? "host-world-changed" : snapshot.localWorld ? "mismatch" : "first-entry", hostWorld.epoch,
         autoloadState.Entries() + 1);
    if (HostWorldEntryAvailable()) {
        if (!clientRejoin.pending) {
            const auto queued = EnterHostWorld("autoload");
            if (queued != EnterResult::Queued && queued != EnterResult::AlreadyQueued) {
                autoloadState.Entered(false, now);
                return false;
            }
        }
        entryApproved = autoloadAwaiting = true;
        Logf("persist: client autoload approved entry epoch=%08x attempt=%u record=%s", hostWorld.epoch, autoloadState.Entries() + 1,
             needsNewHero ? "none (fresh hero)" : RecordKnown() ? "own" : "pending");
        return false;
    }
    return FirstEntryNow(now);
}

} // namespace

bool AutoloadAvailable()
{
    return !net::IsHost() && GetConfig().clientAutoload && rejoinHooks && !ai::ShuttingDown();
}

bool HooksUnsupported()
{
    return installed && (!pairedEnabled || (!net::IsHost() && !rejoinHooks));
}

void HostNameWide(wchar_t (&out)[80])
{
    out[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, steam::HostName(), -1, out, 79);
    if (!*out) wcscpy_s(out, L"the host");
}

wchar_t const* UnsupportedText(wchar_t* buffer, size_t size)
{
    wchar_t detail[96] = {};
    if (hookFailure.compare(0, 4, "pup:") == 0) {
        MultiByteToWideChar(CP_UTF8, 0, pupDigestHex.empty() ? "unknown" : pupDigestHex.substr(0, 16).c_str(), -1, detail, 95);
        _snwprintf_s(buffer, size, _TRUNCATE, i18n::Text(i18n::AdmissionUnsupportedPup), detail);
    } else {
        MultiByteToWideChar(CP_UTF8, 0, hookFailure.empty() ? "native hooks" : hookFailure.c_str(), -1, detail, 95);
        _snwprintf_s(buffer, size, _TRUNCATE, i18n::Text(i18n::AdmissionUnsupportedGame), detail);
    }
    return buffer;
}

void AutoloadCancel(char const* by, bool notify)
{
    if (autoloadState.Held()) return;
    autoloadState.Cancel();
    Logf("persist: client autoload canceled epoch=%08x by=%s; no automatic entry until the host's world changes", hostWorld.epoch, by);
    if (notify) Notice(i18n::ClientAutoloadCanceled);
}

void QueueClientReturn()
{
    recordWaitSince = 0; recordWaitGaveUp = false;
    clientRejoin.Queue();
    resetLocalWorld = true;
    freshLocalHero = false;
    hostAdmissionAck = false;
    net::SetInWorld(net::LocalId(), false);
}

void __fastcall NewGameCommandDetour(void* self, void*)
{
    if (ai::ShuttingDown() || net::IsHost() || !rejoinHooks) {
        originalNewGameCommand(self);
        return;
    }
    AutoloadCancel("manual New Game");
    if (gCSession::GetInstance().GetPlayer() || clientRejoin.pending || captured.valid || requestPending) {
        Logf("persist: client enter state=queued reason=new-game-command epoch=%08x", hostWorld.epoch);
        entryManual = true;
        QueueClientReturn();
        Notice(i18n::ClientReturningToMenu);
        return;
    }
    if (clientWorldEntered && HostWorldEntryAvailable()) {
        entryManual = true;
        if (EnterHostWorld("new-game-command") != EnterResult::Unavailable) Notice(i18n::ClientReturningToMenu);
        return;
    }
    ++menuCommandDepth;
    originalNewGameCommand(self);
    --menuCommandDepth;
}

bool __stdcall NewGameDetour(void* self)
{
    if (ai::ShuttingDown() || net::IsHost() || !rejoinHooks) return originalNewGame(self);
    if (!rejoin::NewGameAllowed(clientWorldEntered)) {
        Log("persist: client New Game refused; this process already entered a world and the in-place native restart froze the game");
        Notice(i18n::ClientRestartToRejoin, true);
        return false;
    }
    if (!worldCleanup || nativeLoadDepth || nativeSaveDepth || captured.valid || requestPending || clientRejoin.pending) {
        Log("persist: client New Game refused; transition or native operation still active");
        Notice(i18n::ClientRejoinFailed, true);
        return false;
    }
    ++nativeLoadDepth;
    nativeNewGame = true;
    ownCharacterReady = false; hostAdmissionAck = false; synced = 0;
    freshLocalHero = false; newWorld = false;
    net::SetInWorld(net::LocalId(), false);
    worldCleanup();
    clock::OnWorldChange();
    crashlog::WorldEntryScope scope("native client HUD.NewGame");
    const bool ok = originalNewGame(self);
    scope.Complete();
    nativeNewGame = false;
    --nativeLoadDepth;
    clientRejoin.Started(ok);
    freshLocalHero = ok; newWorld = ok;
    resetLocalWorld = false;
    if (!ok) {
        restoreFailed = true;
        Log("persist: client native New Game failed; no character admission or save");
        Notice(i18n::ClientRejoinFailed, true);
    } else {
        restoreFailed = false;
        clientWorldEntered = true;
        templateHeroFresh = true;
        autoloadState.Adopted(GetTickCount());
        templateCapture.Want(GetTickCount());
        requestCharacterAgain = receivedNote.epoch != hostWorld.epoch || !receivedWelcome;
        Log("persist: client native New Game returned; fresh local world ready for own account record");
    }
    return ok;
}

rejoin::tmpl::store::Paths TemplatePaths()
{
    return {Path(rejoin::tmpl::Slot, 0), Path(rejoin::tmpl::Slot, 1), Path(rejoin::tmpl::Slot, rejoin::tmpl::Sidecar)};
}

rejoin::tmpl::Identity TemplateIdentity() { return {GameModuleBytes(), saving::Protocol}; }

void TemplateTrace(char const* step, std::string const& path, uint32_t code, bool ok)
{
    Logf("template: file operation=%s path=%s error=%lu ok=%d", step, path.c_str(), static_cast<unsigned long>(code), ok ? 1 : 0);
}

void TemplateInvalidate() { templateCheckedAt = 0; }

rejoin::tmpl::Report TemplateState(bool deep)
{
    namespace tpl = rejoin::tmpl;
    const uint32_t now = GetTickCount();
    if (!deep && templateCheckedAt && now - templateCheckedAt < 500) return templateReport;
    const auto paths = TemplatePaths();
    const auto identity = TemplateIdentity();
    tpl::Facts facts;
    tpl::store::Error error;
    auto report = tpl::store::Check(paths, identity, false, &facts, &error);
    const uint64_t signature = tpl::Signature(facts);
    if (deep && report.health == tpl::Health::Valid && signature != templateVerified) {
        report = tpl::store::Check(paths, identity, true, &facts, &error);
        if (report.health == tpl::Health::Valid) templateVerified = signature;
    }
    if (error.code) LogIo(error);
    templateReport = report;
    templateCheckedAt = now ? now : 1;
    static std::string shown;
    const std::string key = std::string(tpl::HealthName(report.health)) + tpl::WhyName(report.why) + std::to_string(signature) +
                            (deep ? "d" : "");
    if (key != shown) {
        shown = key;
        Logf("template: state=%s why=%s origin=%s header=%llu world=%llu marker=%llu leftovers=%d game=%llu protocol=%u deep=%d folder=%s",
             tpl::HealthName(report.health), tpl::WhyName(report.why), report.sealed ? tpl::OriginName(report.seal.origin) : "-",
             static_cast<unsigned long long>(facts.header.size), static_cast<unsigned long long>(facts.world.size),
             static_cast<unsigned long long>(facts.marker.size),
             (facts.headerTmp.exists || facts.worldTmp.exists || facts.markerTmp.exists) ? 1 : 0,
             static_cast<unsigned long long>(identity.game), static_cast<unsigned>(identity.protocol), deep ? 1 : 0,
             Folder().c_str());
    }
    return report;
}

bool RemoveTemplate(char const* reason)
{
    if (!rejoin::tmpl::OwnSlot(rejoin::tmpl::Slot)) return false;
    rejoin::tmpl::store::Error error;
    const bool clean = rejoin::tmpl::store::RemoveOwn(TemplatePaths(), error, &TemplateTrace);
    Logf("template: remove reason=%s clean=%d operation=%s path=%s error=%lu", reason, clean ? 1 : 0, error.operation,
         error.path.c_str(), static_cast<unsigned long>(error.code));
    TemplateInvalidate();
    return clean;
}

char const* TemplateJobName(TemplateJob job)
{
    return job == TemplateJob::FirstEntry ? "first-entry" : job == TemplateJob::Background ? "background" :
           job == TemplateJob::HostLoad ? "host-load" : "-";
}

void TemplateNotify(char const* code, uint32_t error)
{
    wchar_t text[700]{};
    _snwprintf_s(text, _TRUNCATE, i18n::Text(i18n::ClientTemplateUnavailable), i18n::SaveReason(code), static_cast<unsigned long>(error));
    Logf("template: notice reason=%s error=%lu", code, static_cast<unsigned long>(error));
    NoticeText(text, true);
}

rejoin::Boundary ClientBoundary()
{
    auto& game = gCSession::GetInstance();
    auto* manager = game.GetGUIManager();
    auto* infos = game.IsGameRunning() ? gCInfoManager_PS::GetInstance() : nullptr;
    const bool interaction = !trade::ReadyForSave() || (infos && infos->GetIsRunning()) ||
        (manager && manager->IsAnyPageOpen() &&
         (manager->GetActivePage() == gEGamePage_Trade || manager->GetActivePage() == gEGamePage_Dialog));
    return {nativeLoadDepth != 0 || nativeSaveDepth != 0 || nativeNewGame, processDepth != 0 || menuCommandDepth != 0,
            captured.valid || requestPending, interaction};
}

void TemplateEnded(uint32_t now)
{
    namespace tpl = rejoin::tmpl;
    const auto fail = templatePublish.failure();
    const auto origin = templatePublish.origin();
    const TemplateJob job = templateJob;
    templateJob = TemplateJob::None;
    TemplateInvalidate();
    if (fail == tpl::Fail::None) {
        templateFailures = 0; templateFailCode.clear(); templateFailError = 0;
        if (job == TemplateJob::FirstEntry) templateCapture.Done();
        return;
    }
    RemoveTemplate("rollback");
    templateFailCode = tpl::FailName(fail);
    templateFailError = templateIoError;
    if (origin == tpl::Origin::World) ++templateFailures;
    bool notify = false;
    if (job == TemplateJob::FirstEntry) {
        templateCapture.Failed(now);
        notify = !templateCapture.wanted;
    } else if (job == TemplateJob::Background) notify = templateBackground.Attempts() >= tpl::Background::MaxAttempts;
    Logf("template: failed reason=%s job=%s origin=%s elapsed_ms=%u failures=%u notify=%d", templateFailCode.c_str(),
         TemplateJobName(job), tpl::OriginName(origin), templatePublish.Elapsed(now), templateFailures, notify ? 1 : 0);
    if (notify) TemplateNotify(templateFailCode.c_str(), templateFailError);
}

void TemplatePoll(uint32_t now)
{
    namespace tpl = rejoin::tmpl;
    if (!templatePublish.PollDue(now)) return;
    const auto paths = TemplatePaths();
    const auto facts = tpl::store::Observe(paths, true);
    char line[260];
    SettleText(facts, line, sizeof(line));
    if (templateSettleShown != line) {
        templateSettleShown = line;
        Logf("template: settle +%ums %s", templatePublish.Elapsed(now), line);
    }
    tpl::store::Error error;
    const tpl::Step step = templatePublish.Poll(facts, now);
    switch (step) {
    case tpl::Step::Wait:
        return;
    case tpl::Step::RecoverHeader:
    case tpl::Step::RecoverWorld: {
        if (static_cast<int32_t>(now - templateNextRecover) < 0) return;
        templateNextRecover = now + 500;
        const bool header = step == tpl::Step::RecoverHeader;
        const std::string& target = header ? paths.header : paths.world;
        if (!tpl::store::Recover(target + ".tmp", target, error, &TemplateTrace)) { LogIo(error); templateIoError = error.code; }
        return;
    }
    case tpl::Step::Seal: {
        tpl::Seal seal;
        tpl::Fail why = tpl::Fail::Seal;
        if (!tpl::store::SealFiles(paths, templatePublish.origin(), TemplateIdentity(), seal, error, why)) {
            LogIo(error); templateIoError = error.code;
            templatePublish.Finish(false, why);
            break;
        }
        TemplateInvalidate();
        const auto report = TemplateState(true);
        if (report.health != tpl::Health::Valid) {
            Logf("template: verify failed state=%s why=%s", tpl::HealthName(report.health), tpl::WhyName(report.why));
            templatePublish.Finish(false, tpl::Fail::Seal);
            break;
        }
        Logf("template: ready slot=%s origin=%s header=%llu crc=%08x world=%llu crc=%08x game=%llu protocol=%u settle_ms=%u marker=%s",
             tpl::Slot, tpl::OriginName(seal.origin), static_cast<unsigned long long>(seal.headerSize), seal.headerCrc,
             static_cast<unsigned long long>(seal.worldSize), seal.worldCrc, static_cast<unsigned long long>(seal.game),
             static_cast<unsigned>(seal.protocol), templatePublish.Elapsed(now), paths.marker.c_str());
        templatePublish.Finish(true);
        break;
    }
    case tpl::Step::Failed:
        if (!templateIoError) {
            templateIoError = !facts.header.exists ? facts.header.error : !facts.world.exists ? facts.world.error :
                              facts.header.locked ? facts.header.error : facts.world.locked ? facts.world.error : 0;
        }
        Logf("template: settle timeout reason=%s +%ums %s", tpl::FailName(templatePublish.failure()),
             templatePublish.Elapsed(now), line);
        break;
    }
    if (!templatePublish.Active()) TemplateEnded(now);
}

bool TemplateBackgroundDue(uint32_t now)
{
    namespace tpl = rejoin::tmpl;
    auto& game = gCSession::GetInstance();
    const bool needed = !net::IsHost() && rejoinHooks && originalSave && clientWorldEntered && !templatePublish.Active() &&
                        TemplateState().health != tpl::Health::Valid;
    templateBackground.Observe(needed, now);
    if (!needed || !templateBackground.Due(now)) return false;
    eCEntity* hero = game.GetHero();
    if (!hero || !game.IsGameRunning() || !ownCharacterReady || !net::InWorld(net::LocalId()) || clientRejoin.pending ||
        needsNewHero || NativeSaveReason() || requestPending || captured.valid || !trade::ReadyForSave() ||
        rejoin::State{}.Waiting(ClientBoundary()) != rejoin::Wait::None) return false;
    return static_cast<gEAniState>(Entity(hero).Routine.AniState) == gEAniState_Stand;
}

wchar_t const* AutoloadMenuText(wchar_t* buffer, size_t size)
{
    if (net::IsHost()) return nullptr;
    auto& game = gCSession::GetInstance();
    return AdmissionText(autoloadStatus, game.GetPlayer() != nullptr || game.IsGameRunning(), buffer, size);
}

} // namespace g3t::persist::detail

namespace g3t::persist {

void SetWorldCleanup(void (*cleanup)()) { detail::worldCleanup = cleanup; }
bool ClientTransitionPending() { return !net::IsHost() && detail::clientRejoin.pending; }

bool HostWorldEntryAvailable()
{
    using namespace detail;
    if (net::IsHost() || !rejoinHooks || !originalLoad) return false;
    const auto plan = EntryPlan(ClientQuietNow());
    using R = rejoin::tmpl::Route;
    return (plan.route == R::Template || plan.route == R::Create || plan.route == R::Wait) && FileManager(true) != nullptr;
}

EnterResult EnterHostWorld(char const* reason)
{
    using namespace detail;
    if (net::IsHost()) return EnterResult::NotClient;
    if (!HostWorldEntryAvailable()) {
        const auto plan = EntryPlan(ClientQuietNow());
        Logf("persist: client enter state=unavailable reason=%s template=%s route=%d why=%s hooks=%d", reason ? reason : "-",
             rejoin::tmpl::HealthName(TemplateState().health), static_cast<int>(plan.route), rejoin::tmpl::BlockName(plan.why),
             rejoinHooks ? 1 : 0);
        if (plan.route == rejoin::tmpl::Route::Impossible) TemplateImpossible(plan);
        return EnterResult::Unavailable;
    }
    if (clientRejoin.pending) return EnterResult::AlreadyQueued;
    Logf("persist: client enter state=queued reason=%s epoch=%08x", reason ? reason : "-", hostWorld.epoch);
    QueueClientReturn();
    return EnterResult::Queued;
}

// The engine's own LoadGame of the client template: the only supported way to replace a loaded world.
static void EnterTemplateWorld(uint32_t queuedAt)
{
    using namespace detail;
    auto& game = gCSession::GetInstance();
    auto* manager = game.GetGUIManager();
    void* files = FileManager(true);
    const bCString name(rejoin::tmpl::Slot);
    const auto report = TemplateState(true);
    templateHeroFresh = report.sealed && report.seal.origin == rejoin::tmpl::Origin::NewGame;
    Logf("persist: client enter state=cleanup elapsed_ms=%u epoch=%08x player=%p running=%d template=%s origin=%s",
         GetTickCount() - queuedAt, hostWorld.epoch, game.GetPlayer(), game.IsGameRunning() ? 1 : 0,
         rejoin::tmpl::HealthName(report.health), report.sealed ? rejoin::tmpl::OriginName(report.seal.origin) : "-");
    ++nativeLoadDepth;
    nativeNewGame = true;
    if (manager) manager->ClosePage();
    worldCleanup(); // Every puppet/mirror/lease/camera reference is released while the old entities are valid.
    clock::OnWorldChange();
    ownCharacterReady = false; hostAdmissionAck = false; synced = 0;
    localKind = net::KindMenu; freshLocalHero = false; newWorld = false; resetLocalWorld = false;
    receivedRecord.clear(); receivedPersonal.clear(); receivedNote = {}; receivedWelcome = 0;
    needsNewHero = false; restoreFailed = false;
    templateCapture.Done();
    clientRejoin.Retired();
    rejoinWait = rejoin::Wait::None;
    net::SetInWorld(net::LocalId(), false);
    lowerLoadCalls = 0; lowerLoadGood = true;
    Logf("persist: client enter state=native-load slot=%s elapsed_ms=%u", rejoin::tmpl::Slot, GetTickCount() - queuedAt);
    crashlog::WorldEntryWait("native client template load");
    crashlog::WorldEntryScope scope("native client template HUD.LoadGame");
    const bool hudOk = originalLoad(files, &name);
    scope.Complete();
    nativeNewGame = false;
    --nativeLoadDepth;
    const bool ok = hudOk && lowerLoadCalls == 1 && lowerLoadGood;
    clientRejoin.Started(ok);
    freshLocalHero = ok; newWorld = ok; resetLocalWorld = false;
    requestCharacterAgain = true;
    restoreFailed = !ok;
    Logf("persist: client enter state=%s hud_ok=%d lower_calls=%u lower_ok=%d elapsed_ms=%u player=%p running=%d",
         ok ? "returned" : "failed", hudOk ? 1 : 0, lowerLoadCalls, lowerLoadGood ? 1 : 0, GetTickCount() - queuedAt,
         game.GetPlayer(), game.IsGameRunning() ? 1 : 0);
    if (ok) { clientWorldEntered = true; return; }
    RemoveTemplate("native-load-failed");
    Notice(i18n::ClientRejoinFailed, true);
}

bool ProcessClientTransition()
{
    using namespace detail;
    static uint32_t queuedAt = 0, createWaitSince = 0;
    if (net::IsHost() || ai::ShuttingDown()) return false;
    if (!rejoinHooks) {
        if (installed) {
            const uint32_t at = GetTickCount();
            AdmissionObserve(AutoloadSnapshot(at), at);
        }
        return false;
    }
    static bool startupLogged = false;
    if (!startupLogged && !Folder().empty()) {
        startupLogged = true;
        Logf("template: startup slot=%s folder=%s", rejoin::tmpl::Slot, Folder().c_str());
        TemplateState(true);
    }
    if (ProcessAutoload()) return true;
    const uint32_t now = GetTickCount();
    const bool idleCandidate = !clientRejoin.pending && !templateCapture.wanted && !templatePublish.Active();
    const bool background = idleCandidate && TemplateBackgroundDue(now);
    if (idleCandidate && !background) { queuedAt = 0; return false; }
    if (nativeLoadDepth || nativeSaveDepth || processDepth || menuCommandDepth) return false;
    if (clientRejoin.pending && !queuedAt) queuedAt = now ? now : 1;
    auto& game = gCSession::GetInstance();
    const rejoin::Boundary boundary = ClientBoundary();
    const bool quiet = rejoin::State{}.Waiting(boundary) == rejoin::Wait::None;
    if (templatePublish.Active()) {
        TemplatePoll(now);
        return false;
    }
    if (!clientRejoin.pending) {
        if (background) {
            templateBackground.Tried(now);
            TemplateStart(rejoin::tmpl::Origin::World, TemplateJob::Background);
            return true;
        }
        if (templateCapture.Expired(now)) {
            templateCapture.Done();
            Log("template: save abandoned; no quiet boundary within the hold window");
            return false;
        }
        if (!templateCapture.Due(now) || !game.GetHero() || !game.IsGameRunning() || !quiet) return false;
        templateCapture.Began();
        TemplateStart(rejoin::tmpl::Origin::NewGame, TemplateJob::FirstEntry);
        return true;
    }
    const auto wait = clientRejoin.Waiting(boundary);
    if (wait != rejoinWait) {
        rejoinWait = wait;
        Logf("persist: client enter state=boundary-wait wait=%d elapsed_ms=%u", static_cast<int>(wait), now - queuedAt);
        Notice(wait == rejoin::Wait::Save ? i18n::ClientRejoinWaitSave :
               wait == rejoin::Wait::Interaction ? i18n::ClientRejoinWaitTrade :
               i18n::ClientReturningToMenu);
    }
    if (!worldCleanup || !clientRejoin.CanRetire(boundary)) return false;
    if (!entryManual && !entryApproved) return false;
    using R = rejoin::tmpl::Route;
    const auto plan = EntryPlan(quiet);
    if (plan.route == R::Create) {
        createWaitSince = 0;
        TemplateStart(rejoin::tmpl::Origin::World, TemplateJob::HostLoad);
        return true;
    }
    if (plan.route == R::Wait) {
        if (plan.why == rejoin::tmpl::Block::Record) {
            if (!recordWaitSince) recordWaitSince = now ? now : 1;
            if (now - recordWaitSince >= 12000 && !recordWaitGaveUp) {
                recordWaitGaveUp = true;
                Logf("template: record wait gave up after %u ms; entering without the host's answer", now - recordWaitSince);
            }
            return false;
        }
        if (plan.why != rejoin::tmpl::Block::NotQuiet) { createWaitSince = 0; return false; }
        if (!createWaitSince) createWaitSince = now ? now : 1;
        if (now - createWaitSince < 8000) return false;
        const char* reason = NativeSaveReason();
        templateFailCode = reason ? reason : "not-running";
        templateFailError = 0;
        createWaitSince = 0;
        entryManual = entryApproved = false;
        const uint32_t startedAt = queuedAt;
        queuedAt = 0;
        clientRejoin.Retired();
        clientRejoin.failed = true;
        rejoinWait = rejoin::Wait::None;
        Logf("persist: client enter state=failed reason=template-save-refused detail=%s elapsed_ms=%u", templateFailCode.c_str(),
             now - startedAt);
        TemplateNotify(templateFailCode.c_str(), 0);
        return false;
    }
    createWaitSince = 0;
    const uint32_t startedAt = queuedAt;
    if (plan.route == R::Template) {
        if (TemplateState(true).health != rejoin::tmpl::Health::Valid) {
            RemoveTemplate("deep-check-failed");
            return false;
        }
        const bool manualEntry = entryManual;
        entryManual = entryApproved = false;
        queuedAt = 0;
        recordWaitSince = 0; recordWaitGaveUp = false;
        EnterTemplateWorld(startedAt);
        if (manualEntry && clientRejoin.fresh) autoloadState.Adopted(GetTickCount());
        return true;
    }
    entryManual = entryApproved = false;
    queuedAt = 0;
    recordWaitSince = 0; recordWaitGaveUp = false;
    clientRejoin.Retired();
    rejoinWait = rejoin::Wait::None;
    Logf("persist: client enter state=failed reason=no-route route=%d why=%s elapsed_ms=%u", static_cast<int>(plan.route),
         rejoin::tmpl::BlockName(plan.why), now - startedAt);
    if (plan.route == R::Impossible) {
        clientRejoin.failed = true;
        TemplateImpossible(plan);
    }
    return false;
}

} // namespace g3t::persist
