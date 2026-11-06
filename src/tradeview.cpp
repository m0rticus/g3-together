// SPDX-License-Identifier: GPL-3.0-or-later
#include "tradeview.h"

#include "ai.h"
#include "log.h"
#include "remote.h"
#include "trade.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace g3t::tradeview {
namespace {
constexpr uintptr_t PredicateRva = 0xA1570;
constexpr uintptr_t TradeListVTableRva = 0x2D6D44;
constexpr size_t Stolen = 6;
constexpr uint8_t PredicateSignature[32] = {
    0x56,0x57,0x8b,0x7c,0x24,0x0c,0x85,0xff,0x8b,0xf1,0x7c,0x08,0x8b,0x46,0x14,0x3b,
    0x78,0x34,0x7c,0x07,0x5f,0x32,0xc0,0x5e,0xc2,0x04,0x00,0x73,0xf7,0x8b,0x40,0x30
};

using VisibleFn = GEBool (__fastcall *)(void*, void*, int);

VisibleFn originalVisible = nullptr;
unsigned char* predicate = nullptr;
unsigned char* trampoline = nullptr;
uintptr_t tradeListVTable = 0;
uint8_t originalBytes[Stolen] = {};
uint8_t patchedBytes[Stolen] = {};
bool installed = false;
bool failed = false;

void Unsupported(char const* message)
{
    if (failed) return;
    failed = true;
    Log(message);
}

GEBool __fastcall Filter(void* control, void*, int stack)
{
    if (ai::ShuttingDown()) return originalVisible ? originalVisible(control, nullptr, stack) : GETrue;
    if (!originalVisible) return GEFalse;
    const GEBool visible = originalVisible(control, nullptr, stack);
    if (!visible) return GEFalse;
    if (!control) return GETrue;

    uintptr_t primaryVTable = 0;
    std::memcpy(&primaryVTable, control, sizeof(primaryVTable));
    if (primaryVTable != tradeListVTable) return GETrue;

    gCSession& session = gCSession::GetInstance();
    gCGUIManager* guiManager = session.GetGUIManager();
    if (!guiManager || guiManager->GetActivePage() != gEGamePage_Trade) return GETrue;

    gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
    if (!infos || !infos->AccessIsRunning()) return GETrue;
    eCEntity* partner = infos->GetDialogPartnerNPC();
    if (!partner || remote::PuppetOwner(partner) == 0) return GETrue;

    eCDynamicEntity* hero = session.GetHero();
    if (!hero) return GETrue;

    gCInventory_PS* inventory = nullptr;
    std::memcpy(&inventory, static_cast<uint8_t*>(control) + 0x14, sizeof(inventory));
    if (!inventory || inventory->GetEntity() != hero) return GETrue;

    Entity self(hero);
    return trade::TradableStack(self, stack) ? GETrue : GEFalse;
}
}

void Install()
{
    if (installed || failed) return;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA("Game.dll"));
    if (!base) {
        Unsupported("trade: unsupported native trade-grid visibility hook; Game.dll is unavailable");
        return;
    }

    unsigned char* target = base + PredicateRva;
    if (std::memcmp(target, PredicateSignature, sizeof(PredicateSignature)) != 0) {
        Unsupported("trade: unsupported native trade-grid visibility hook; predicate signature mismatch");
        return;
    }
    uintptr_t vtablePredicate = 0;
    std::memcpy(&vtablePredicate, base + TradeListVTableRva + 0x10c, sizeof(vtablePredicate));
    if (vtablePredicate != reinterpret_cast<uintptr_t>(target)) {
        Unsupported("trade: unsupported native trade-grid visibility hook; trade-list vtable mismatch");
        return;
    }

    std::memcpy(originalBytes, target, Stolen);
    void* installedTrampoline = ai::JumpTo(target, reinterpret_cast<void*>(&Filter), Stolen);
    if (!installedTrampoline) {
        Unsupported("trade: unsupported native trade-grid visibility hook; could not install predicate trampoline");
        return;
    }

    std::memcpy(patchedBytes, target, Stolen);
    if (target[0] != 0xe9 || target[5] != 0x90) {
        DWORD old = 0;
        if (VirtualProtect(target, Stolen, PAGE_EXECUTE_READWRITE, &old)) {
            std::memcpy(target, originalBytes, Stolen);
            DWORD ignored = 0;
            VirtualProtect(target, Stolen, old, &ignored);
        }
        // ai::RestorePatches owns the executable allocation; this pointer is the original trampoline, not the allocation base.
        Unsupported("trade: unsupported native trade-grid visibility hook; predicate patch verification failed");
        return;
    }

    predicate = target;
    trampoline = static_cast<unsigned char*>(installedTrampoline);
    originalVisible = reinterpret_cast<VisibleFn>(trampoline);
    tradeListVTable = reinterpret_cast<uintptr_t>(base + TradeListVTableRva);
    installed = true;
    Log("trade: native local player-trade grid visibility hook installed");
}

void Uninstall()
{
    // Restoration is owned by ai::RestorePatches; never free the executable trampoline
    // because a native caller may have entered it during teardown.
    if (!installed) return;
    predicate = nullptr;
    originalVisible = nullptr;
    tradeListVTable = 0;
    installed = false;
    Log("trade: native local player-trade grid visibility hook uninstalled");
}
}
