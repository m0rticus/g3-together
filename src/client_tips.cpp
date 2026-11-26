// SPDX-License-Identifier: GPL-3.0-or-later
#include "client_tips.h"
#include "client_tips_policy.h"

#include "log.h"

#include <GFC.h>
#include <Game.h>
#include <Windows.h>
#include <cstring>

namespace g3t::tips {
namespace {

policy::JoinDefaultTracker tracker;
uint32_t worldEntrySeq = 0;
bool layoutChecked = false;
bool layoutSupported = false;

bool ReadableBytes(void const* address, size_t size)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (!start || size > UINTPTR_MAX - start) return false;
    const uintptr_t end = start + size;
    for (uintptr_t at = start; at < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void const*>(at), &info, sizeof(info)) || info.State != MEM_COMMIT)
            return false;
        const DWORD protect = info.Protect & 0xff;
        if ((info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            (protect != PAGE_READONLY && protect != PAGE_READWRITE && protect != PAGE_WRITECOPY &&
             protect != PAGE_EXECUTE_READ && protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY))
            return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= at) return false;
        at = next;
    }
    return true;
}

bool VerifyTutorialCheckBoxLayout()
{
    if (layoutChecked) return layoutSupported;
    HMODULE game = GetModuleHandleA("Game.dll");
    HMODULE gfc = GetModuleHandleA("GFC.dll");
    if (!game || !gfc) return false;
    layoutChecked = true;
    FARPROC openTutorial = GetProcAddress(game, "?OpenTutorial@gCGUIManager@@QAE_NABVbCUnicodeString@@@Z");
    FARPROC getCheck = GetProcAddress(gfc, "?GetCheck@CFFGFCCheckBox@@QBE_NXZ");
    FARPROC setCheck = GetProcAddress(gfc, "?SetCheck@CFFGFCCheckBox@@QAEX_N@Z");
    uint32_t iatVa = 0;
    if (!openTutorial || !getCheck || !setCheck ||
        !ReadableBytes(reinterpret_cast<void const*>(openTutorial), policy::OpenTutorialProbeBytes) ||
        !policy::VerifyOpenTutorialBytes(reinterpret_cast<uint8_t const*>(openTutorial),
                                         policy::OpenTutorialProbeBytes, iatVa) ||
        !ReadableBytes(reinterpret_cast<void const*>(static_cast<uintptr_t>(iatVa)), sizeof(FARPROC))) {
        Log("tips: ANOMALY OpenTutorial layout mismatch; tutorial checkbox sync unavailable");
        return false;
    }
    FARPROC importedGetCheck = nullptr;
    std::memcpy(&importedGetCheck, reinterpret_cast<void const*>(static_cast<uintptr_t>(iatVa)), sizeof(importedGetCheck));
    layoutSupported = importedGetCheck == getCheck;
    Log(layoutSupported ? "tips: native OpenTutorial tutorial checkbox layout ready"
                        : "tips: ANOMALY OpenTutorial GetCheck import mismatch; tutorial checkbox sync unavailable");
    return layoutSupported;
}

bool EnsureCheckBoxChecked(bool& changed)
{
    changed = false;
    if (!VerifyTutorialCheckBoxLayout()) return false;
    gCGUIManager* manager = gCSession::GetInstance().GetGUIManager();
    if (!manager || !ReadableBytes(manager, policy::GuiRootOffset + sizeof(void*))) return false;
    unsigned char* root = nullptr;
    std::memcpy(&root, reinterpret_cast<unsigned char const*>(manager) + policy::GuiRootOffset, sizeof(root));
    if (!root || !ReadableBytes(root, policy::TutorialPageOffset + sizeof(void*))) return false;
    unsigned char* tutorialPage = nullptr;
    std::memcpy(&tutorialPage, root + policy::TutorialPageOffset, sizeof(tutorialPage));
    if (!tutorialPage || !ReadableBytes(tutorialPage, policy::TutorialCheckBoxOffset + sizeof(void*))) return false;
    auto* checkBox = reinterpret_cast<CFFGFCCheckBox*>(tutorialPage + policy::TutorialCheckBoxOffset);
    if (!checkBox->GetCheck()) {
        checkBox->SetCheck(GETrue);
        changed = true;
    }
    return true;
}

bool ApplyDefault(char const* reason, eCEntity* hero, uint32_t welcomeSerial)
{
    if (!hero) return false;
    auto* memory = static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory));
    if (!memory) return false;
    bool memoryChanged = false;
    if (!memory->GetHideTips()) {
        memory->SetHideTips(GETrue);
        memoryChanged = true;
    }
    bool boxChanged = false;
    const bool boxOk = EnsureCheckBoxChecked(boxChanged);
    if (memoryChanged || boxChanged) {
        Logf("tips: client hide_tips enabled reason=%s welcome=%u world=%u memory_changed=%d checkbox_ok=%d checkbox_changed=%d",
             reason ? reason : "-", welcomeSerial, worldEntrySeq, memoryChanged ? 1 : 0, boxOk ? 1 : 0, boxChanged ? 1 : 0);
    }
    return true;
}

} // namespace

void OnWorldActivate() { ++worldEntrySeq; }

void PrepareForWorldEntry(char const* reason)
{
    bool boxChanged = false;
    if (EnsureCheckBoxChecked(boxChanged) && boxChanged)
        Logf("tips: pre-entry tutorial checkbox checked reason=%s", reason ? reason : "-");
}

void OnWorldEntered(char const* reason, eCEntity* hero, uint32_t welcomeSerial)
{
    const policy::State state{false, true, hero != nullptr, welcomeSerial, worldEntrySeq,
                              reinterpret_cast<uintptr_t>(hero)};
    if (ApplyDefault(reason, hero, welcomeSerial)) tracker.MarkApplied(state);
}

void Tick(bool isHost, bool joined, bool inRunningWorld, eCEntity* hero, uint32_t welcomeSerial)
{
    const policy::State state{isHost, joined, inRunningWorld, welcomeSerial, worldEntrySeq,
                              reinterpret_cast<uintptr_t>(hero)};
    if (!tracker.ShouldApply(state)) return;
    if (ApplyDefault("client-join", hero, welcomeSerial)) tracker.MarkApplied(state);
}

} // namespace g3t::tips
