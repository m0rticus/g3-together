// SPDX-License-Identifier: GPL-3.0-or-later
#include "hud.h"

#include "ai.h"
#include "downed.h"
#include "log.h"
#include "net.h"
#include "persist.h"
#include "remote.h"
#include "movement.h"
#include "options.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace g3t::i18n {
char const* GameTextLanguage() { return eCLocAdmin::GetInstance().GetCurrentLanguage().GetText(); }
}

namespace g3t::hud {
namespace {
unsigned count = 0;
struct Notice { Kind kind; std::wstring text; };
std::mutex noticeMutex;
std::vector<Notice> pending;

// CP 1.75 Game.dll: GUI2's wrapper activates the HUD window at +0x7cda5.
// Append directly to the same text list, with its original palette and layout.
constexpr uintptr_t AppendRva = 0xa76d0;
constexpr unsigned char RootLayout[] = {0x8b, 0x43, 0x04};
constexpr unsigned char ListLayout[] = {
    0x8b, 0x40, 0x7c, 0x85, 0xc0, 0x74, 0x36, 0x8d, 0xb0, 0x88, 0x02, 0x00, 0x00, 0x8b, 0xce
};
constexpr unsigned char AppendArguments[] = {
    0xd9, 0x44, 0x24, 0x20, 0x8b, 0x54, 0x24, 0x24, 0x8d, 0x44, 0x24, 0x2c, 0x50, 0x51,
    0x8b, 0x4c, 0x24, 0x30, 0xd9, 0x1c, 0x24, 0x51, 0x52, 0x8d, 0x44, 0x24, 0x1c, 0x50,
    0x8d, 0x4c, 0x24, 0x1c, 0x51, 0x8b, 0xc6, 0xe8, 0xfd, 0xa8, 0x02, 0x00
};
constexpr unsigned char AppendEntry[] = {
    0x83, 0xec, 0x08, 0x55, 0x8b, 0x6c, 0x24, 0x14, 0x56, 0x57, 0x8b, 0xf8, 0x8b, 0x47, 0x14,
    0x33, 0xd2, 0x3b, 0xc2, 0x89, 0x54, 0x24, 0x1c, 0x7e, 0x24
};
constexpr unsigned char AppendReturn[] = {0x5e, 0x5d, 0x83, 0xc4, 0x08, 0xc2, 0x18, 0x00};
unsigned char* presentationGame = nullptr;
void* appendText = nullptr;
bool presentationChecked = false, presentationWaiting = false;

// Native ABI: list in EAX, six stack arguments, ret 0x18. Remove only the
// extra list argument; the native callee returns directly to our stdcall caller.
__declspec(naked) void __stdcall AppendText(void*, bCUnicodeString const&, bCFloatAlphaColor const&,
                                            GEInt, GEInt, GEFloat, bCVector2 const*)
{
    __asm {
        mov eax, [esp + 4]
        pop edx
        add esp, 4
        push edx
        jmp dword ptr [appendText]
    }
}

bool BindPresentation()
{
    if (appendText) return true;
    if (presentationChecked) return false;
    HMODULE game = GetModuleHandleA("Game.dll");
    if (!game) return false;
    presentationChecked = true;
    auto* bytes = reinterpret_cast<unsigned char*>(game);
    if (GetProcAddress(game, "?PrintGameMessage@gCGUIManager@@QAEXABVbCUnicodeString@@W4gEGameMessageType@@@Z") !=
            reinterpret_cast<FARPROC>(bytes + 0x7c5c0) ||
        std::memcmp(bytes + 0x7cd3c, RootLayout, sizeof(RootLayout)) ||
        std::memcmp(bytes + 0x7cd96, ListLayout, sizeof(ListLayout)) ||
        std::memcmp(bytes + 0x7cdab, AppendArguments, sizeof(AppendArguments)) ||
        std::memcmp(bytes + AppendRva, AppendEntry, sizeof(AppendEntry)) ||
        std::memcmp(bytes + 0xa77d2, AppendReturn, sizeof(AppendReturn))) {
        Log("hud: ANOMALY passive message-list ABI mismatch; notices retained, no activating fallback");
        return false;
    }
    presentationGame = bytes;
    appendText = bytes + AppendRva;
    Log("hud: passive native message list ready; GUI activation and script dispatch bypassed");
    return true;
}

void* MessageList()
{
    gCGUIManager* manager = gCSession::GetInstance().GetGUIManager();
    if (!manager) return nullptr;
    void* root = nullptr;
    std::memcpy(&root, reinterpret_cast<unsigned char*>(manager) + 4, sizeof(root));
    if (!root) return nullptr;
    void* owner = nullptr;
    std::memcpy(&owner, static_cast<unsigned char*>(root) + 0x7c, sizeof(owner));
    return owner ? static_cast<unsigned char*>(owner) + 0x288 : nullptr;
}

bool Display(Notice const& notice, void* list)
{
    uintptr_t colorRva = 0;
    switch (notice.kind) {
    case Info: colorRva = 0x3f2aac; break;
    case Good: colorRva = 0x3f2a94; break;
    case Warning: colorRva = 0x3f2ab8; break;
    case Bad: colorRva = 0x3f2a88; break;
    default:
        Logf("hud: ANOMALY invalid notice kind=%d; text not displayed", static_cast<int>(notice.kind));
        return false;
    }
    const bCFloatAlphaColor color(*reinterpret_cast<bCByteAlphaColor const*>(presentationGame + colorRva));
    const bCVector2 offset(0.0f, -20.0f);
    AppendText(list, bCUnicodeString(notice.text.c_str()), color, 10, 29, 6.0f, &offset);
    return true;
}

struct Snapshot { uintptr_t body; std::string text; };
struct Observation {
    unsigned serial;
    Kind kind;
    DWORD at;
    uintptr_t body;
    bool sampled100 = false;
};
std::vector<Observation> observations;

Snapshot Capture()
{
    gCSession& session = gCSession::GetInstance();
    eCEntity* hero = session.GetHero();
    eCEntity* body = remote::ControlledBody();
    if (!body) body = hero;
    if (!body) return {0, "hero=none"};
    Entity player(body);
    auto* routine = static_cast<gCScriptRoutine_PS*>(body->GetPropertySet(eEPropertySetType_ScriptRoutine));
    auto* movement = static_cast<gCCharacterMovement_PS*>(body->GetPropertySet(eEPropertySetType_CharacterMovement));
    auto* focus = static_cast<gCFocus_PS*>(body->GetPropertySet(eEPropertySetType_Focus));
    const bCVector& position = body->GetWorldPosition();
    const bCQuaternion& rotation = body->GetWorldRotation();
    const bCVector velocity = movement ? movement->GetLocalVelocity() : bCVector(0.0f, 0.0f, 0.0f);
    CFFGUIModule& guiModule = CFFGUIModule::GetInstance();
    char text[760];
    Format(text, "hero=%p body=%p held=%d terminal=%d can_move=%d session_input=%d device_input=%d gui_input=%d "
                 "gui_active=%p gui_focus=%p camera=%p task=%.48s state=%.48s ai=%d action=%d ani=%d "
                 "pos=%.1f,%.1f,%.1f rot=%.3f,%.3f,%.3f,%.3f vel=%.1f,%.1f,%.1f "
                 "move=%d moving=%d player_control=%d move_enabled=%d focus_lock=%d",
           hero, body, downed::Held() ? 1 : 0, downed::Terminal() ? 1 : 0,
           session.CanPlayerMove() ? 1 : 0, session.IsInputEnabled() ? 1 : 0,
           eCApplication::GetInstance().IsInputDeviceEnabled() ? 1 : 0, guiModule.IsInputEnabled() ? 1 : 0,
           guiModule.GetActiveWnd(), guiModule.GetFocusWnd(), session.GetCamera_PS(),
           routine ? player.Routine.GetCurrentTask().GetText() : "-",
           routine ? player.Routine.GetCurrentState().GetText() : "-",
           routine ? static_cast<int>(player.Routine.AIMode) : -1,
           routine ? static_cast<int>(player.Routine.Action) : -1,
           routine ? static_cast<int>(player.Routine.AniState) : -1,
           position.GetX(), position.GetY(), position.GetZ(),
           rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW(),
           velocity.GetX(), velocity.GetY(), velocity.GetZ(),
           movement ? static_cast<int>(movement->GetMovementMode()) : -1,
           movement && movement->IsInMovingState(), movement && movement->GetMovementIsControledByPlayer(),
           movement && movement->IsMovementEnabledFromSPU(), focus && focus->GetFocusLocked());
    return {reinterpret_cast<uintptr_t>(body), text};
}

void SampleDue(DWORD now)
{
    for (auto it = observations.begin(); it != observations.end();) {
        const DWORD elapsed = now - it->at;
        if (elapsed < 100 || (it->sampled100 && elapsed < 500)) { ++it; continue; }
        const Snapshot snapshot = Capture();
        if (snapshot.body != it->body) {
            Logf("hud-motion: notice=%u kind=%d phase=cancelled elapsed_ms=%lu reason=local-body-changed",
                 it->serial, static_cast<int>(it->kind), elapsed);
            it = observations.erase(it);
            continue;
        }
        if (!it->sampled100) {
            Logf("hud-motion: notice=%u kind=%d phase=100ms elapsed_ms=%lu %s",
                 it->serial, static_cast<int>(it->kind), elapsed, snapshot.text.c_str());
            it->sampled100 = true;
        }
        if (elapsed >= 500) {
            Logf("hud-motion: notice=%u kind=%d phase=500ms elapsed_ms=%lu %s",
                 it->serial, static_cast<int>(it->kind), elapsed, snapshot.text.c_str());
            it = observations.erase(it);
        } else {
            ++it;
        }
    }
}
}

void Message(Kind kind, wchar_t const* text)
{
    std::lock_guard<std::mutex> lock(noticeMutex);
    pending.push_back({kind, text});
}

void Tick()
{
    if (ai::ShuttingDown() || persist::NativeLoading() || !gCSession::GetInstance().IsGameRunning()) return;
    movement::Stage stage("passive HUD");
    SampleDue(GetTickCount());
    {
        std::lock_guard<std::mutex> lock(noticeMutex);
        if (pending.empty()) return;
    }
    void* list = BindPresentation() ? MessageList() : nullptr;
    if (!list) {
        if (!presentationWaiting) Log("hud: presentation unavailable; notices queued until the passive HUD is ready");
        presentationWaiting = true;
        return;
    }
    presentationWaiting = false;
    std::vector<Notice> batch;
    {
        std::lock_guard<std::mutex> lock(noticeMutex);
        batch.swap(pending);
    }
    for (auto const& notice : batch) {
        const Snapshot before = Capture();
        const auto start = std::chrono::steady_clock::now();
        if (!Display(notice, list)) continue;
        const auto displayUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        const DWORD at = GetTickCount();
        const Snapshot after = Capture();
        char utf8[700] = {};
        WideCharToMultiByte(CP_UTF8, 0, notice.text.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        unsigned serial = 0;
        {
            std::lock_guard<std::mutex> lock(noticeMutex);
            serial = ++count;
        }
        Logf("hud: [%d] %s", static_cast<int>(notice.kind), utf8);
        Logf("hud-motion: notice=%u kind=%d role=%s phase=before %s",
             serial, static_cast<int>(notice.kind), net::IsHost() ? "host" : "client", before.text.c_str());
        Logf("hud-motion: notice=%u kind=%d phase=after elapsed_ms=0 display_us=%lld %s",
             serial, static_cast<int>(notice.kind), displayUs, after.text.c_str());
        observations.push_back({serial, notice.kind, at, after.body});
    }
}

void OnWorldChange()
{
    if (!observations.empty())
        Logf("hud-motion: phase=cancelled count=%zu reason=world-change", observations.size());
    observations.clear();
}

bool InventoryOpen()
{
    gCGUIManager* manager = gCSession::GetInstance().GetGUIManager();
    return manager && manager->IsAnyPageOpen() && manager->GetActivePage() == gEGamePage_Inventory;
}

namespace {
// A main menu element by its id (QA's menu scan: the Community Patch's version line is 20812, top centre).
CFFGFCWnd* MenuElement(GEU32 id)
{
    CFFGUIModule& module = CFFGUIModule::GetInstance();
    for (int y = 8; y < 768; y += 64)
        for (int x = 8; x < 1024; x += 64)
            for (CFFGFCWnd* window = CFFGFCWnd::FromHandle(module.WindowFromPoint(bCPoint(x, y), 0)); window; window = window->GetParentWindow()) {
                if (window->GetDlgCtrlID() == id) return window;
                if (CFFGFCWnd* child = window->GetDlgItem(id)) return child;
            }
    return nullptr;
}

constexpr GEU32 VersionLine = 20812;
std::wstring lineText;   // the version line as the game made it
bCRect lineRect;
bool lineSaved = false, lineChanged = false;
}

bool ClientNewGameEnabled(bool enabled)
{
    if (net::IsHost()) return false;
    auto* manager = gCSession::GetInstance().GetGUIManager();
    if (!manager || !manager->IsMenuOpen()) return false;
    CFFGFCWnd* button = MenuElement(0x505b); // Verified CP New Game control, not Load or Save.
    if (!button) return false;
    button->EnableWindow(enabled);
    return true;
}

// In the main menu the game's messages do not show (menutextdiag 24.10.: GUI2, the GUI manager's message and its subtitle
// line all stayed invisible), but its top line is an element: the Community Patch's version line. A second line under it,
// across the whole width, says what a player waiting there must know; the version stays as it is.
bool MenuLine(wchar_t const* text)
{
    if (!text && !lineChanged) return true;
    CFFGFCWnd* line = MenuElement(VersionLine);
    if (!line) return false;
    if (!lineSaved) {
        lineText = line->GetWindowTextA().GetText();
        line->GetWindowRect(lineRect);
        lineSaved = true;
    }
    if (!text) {
        line->SetWindowTextA(bCUnicodeString(lineText.c_str()));
        line->MoveWindow(lineRect);
        lineChanged = false;
        return true;
    }
    std::wstring content(text);
    if (auto* adopted = options::Hint()) {
        content += L"\n";
        content += adopted;
    }
    const std::wstring shown = lineText + L"\n" + content;
    if (line->GetWindowTextA().GetText() != shown) {
        line->SetWindowTextA(bCUnicodeString(shown.c_str()));
        char utf8[900] = {};
        WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        Logf("menu: %s", utf8);
    }
    // Room for the version and two lines of text: a refusal that lists differing files, or German, runs longer.
    line->MoveWindow(bCRect(0, lineRect.GetTop(), 1024,
        lineRect.GetTop() + (options::Hint() ? 5 : 3) * lineRect.GetHeight() + 4));
    lineChanged = true;
    return true;
}

}
