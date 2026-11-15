// SPDX-License-Identifier: GPL-3.0-or-later
#include "options.h"

#include "ai.h"
#include "config.h"
#include "hud.h"
#include "i18n.h"
#include "log.h"
#include "persist.h"

#include <Game.h>
#include <Windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>

namespace g3t::options {
namespace {
policy::Session session;
std::atomic<bool> sessionActive{false};
bool installed = false;
std::wstring hint;
std::mutex sessionMutex;
bool reentry = false;
using SetupFn = eSSetupEngine const& (__thiscall*)(eCApplication const*);
SetupFn originalSetup = nullptr;
using StringFn = GEBool (__thiscall*)(eCConfigFile const*, bCString const&, bCString const&, bCString&);
StringFn originalString = nullptr;
uintptr_t gameBase = 0;
thread_local unsigned hostNpcJudgement = 0;

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

bool ModuleHasRange(HMODULE module, size_t offset, size_t size)
{
    auto const* image = reinterpret_cast<unsigned char const*>(module);
    if (!ReadableBytes(image, sizeof(IMAGE_DOS_HEADER))) return false;
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096 ||
        !ReadableBytes(image + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS32)))
        return false;
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    return offset <= imageSize && size <= imageSize - offset && ReadableBytes(image + offset, size);
}

eSSetupEngine const& LocalSetup()
{
    auto& app = eCApplication::GetInstance();
    return originalSetup ? originalSetup(&app) : app.GetEngineSetup();
}

eSSetupEngine const& __fastcall Setup(eCApplication const* app, void*)
{
    auto const& local = originalSetup(app);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!session.active || policy::IsSetupSerializer(caller - gameBase)) return local;
    // Copy each calling thread's live local UI/difficulty fields, overlay only gameplay.
    thread_local eSSetupEngine effective;
    effective = local;
    policy::ApplySetup(effective, session.host);
    if (hostNpcJudgement) effective.Difficulty = bCString(session.host.difficulty);
    return effective;
}

GEBool __fastcall String(eCConfigFile const* config, void*, bCString const& section,
                        bCString const& key, bCString& value)
{
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        if (session.active && !_stricmp(section.GetText(), "Game") && !_stricmp(key.GetText(), "PlayerStart")) {
            value = bCString(session.host.playerStart);
            return GETrue;
        }
    }
    return originalString(config, section, key, value);
}

void Append(std::wstring& text, wchar_t const* value)
{
    if (!text.empty()) text += L", ";
    text += value;
}

void Summary(unsigned changed, policy::Snapshot const& s)
{
    if (!changed) {
        std::lock_guard<std::mutex> lock(sessionMutex);
        hint.clear();
        return;
    }
    std::wstring list;
    wchar_t part[180] = {};
    auto number = [&](unsigned bit, i18n::Message message, auto value) {
        if (!(changed & bit)) return;
        _snwprintf_s(part, _TRUNCATE, i18n::Text(message), value);
        Append(list, part);
    };
    number(policy::Balancing, i18n::OptionsBalancing, i18n::Text(s.balancing ? i18n::OptionsOn : i18n::OptionsOff));
    number(policy::AI, i18n::OptionsAI, i18n::Text(s.ai ? i18n::OptionsOn : i18n::OptionsOff));
    number(policy::MinHit, i18n::OptionsMinHit, static_cast<int>(s.minHit));
    number(policy::XP, i18n::OptionsXP, s.xp);
    number(policy::FriendlyFire, i18n::OptionsFriendlyFire, s.friendlyFire);
    number(policy::NpcDamage, i18n::OptionsNpcDamage, static_cast<double>(s.npcDamage));
    number(policy::NpcHp, i18n::OptionsNpcHp, static_cast<double>(s.npcHp));
    if (changed & policy::PlayerStart) {
        wchar_t name[64] = {};
        MultiByteToWideChar(CP_UTF8, 0, s.playerStart, -1, name, 64);
        number(policy::PlayerStart, i18n::OptionsPlayerStart, name);
    }
    wchar_t difficulty[64] = {}, text[900] = {};
    MultiByteToWideChar(CP_UTF8, 0, LocalSetup().Difficulty.GetText(), -1, difficulty, 64);
    _snwprintf_s(text, _TRUNCATE, i18n::Text(i18n::OptionsAdopted), list.c_str(), difficulty);
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        hint = text;
    }
    hud::Message(hud::Good, text);
    char utf8[2400] = {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
    Logf("options: %s", utf8);
}
}

bool Install()
{
    if (installed) return true;
    HMODULE engineModule = GetModuleHandleA("Engine.dll");
    HMODULE gameModule = GetModuleHandleA("Game.dll");
    void* game = gameModule;
    auto* entry = engineModule ? reinterpret_cast<unsigned char*>(GetProcAddress(engineModule,
        "?GetEngineSetup@eCApplication@@QBEABUeSSetupEngine@@XZ")) : nullptr;
    const unsigned char getter[] = {0x8d, 0x81, 0x98, 0, 0, 0, 0xc3};
    if (!game || !entry || !ReadableBytes(entry, sizeof(getter)) || std::memcmp(entry, getter, sizeof(getter))) {
        Log("options: native setup getter unavailable/changed; host balancing delegation cannot start");
        return false;
    }
    gameBase = reinterpret_cast<uintptr_t>(game);
    // Verify the one getter-based writer before allowing an overlay there.
    const unsigned char writer[] = {0x0f, 0xb6, 0x80, 0xea, 0x01, 0, 0, 0x50};
    if (!ModuleHasRange(gameModule, 0x90b0e, sizeof(writer)) ||
        std::memcmp(reinterpret_cast<void const*>(gameBase + 0x90b0e), writer, sizeof(writer))) {
        Log("options: native setup serialization site changed; delegation cannot start");
        return false;
    }
    originalString = reinterpret_cast<StringFn>(ai::PatchImport(game, "Engine.dll",
        "?GetValue@eCConfigFile@@QBE_NABVbCString@@0AAV2@@Z", reinterpret_cast<void*>(&String)));
    if (!originalString) {
        Log("options: native PlayerStart reader unavailable; delegation cannot start");
        return false;
    }
    originalSetup = reinterpret_cast<SetupFn>(ai::JumpTo(entry, reinterpret_cast<void*>(&Setup), 6));
    installed = originalSetup != nullptr;
    Logf("options: getter overlays installed=%d setup=%p PlayerStart=%p disk_writes=0", installed, originalSetup, originalString);
    return installed;
}

policy::Snapshot Capture()
{
    auto const& setup = LocalSetup();
    auto const& config = GetLocalConfig();
    policy::Snapshot s;
    s.balancing = setup.AlternativeBalancing ? 1 : 0;
    s.ai = setup.AlternativeAI ? 1 : 0;
    s.minHit = setup.MinHitDuration >= 0 && setup.MinHitDuration <= 15
        ? static_cast<int16_t>(setup.MinHitDuration) : -1;
    s.xp = setup.XPModifier;
    s.friendlyFire = config.friendlyFire;
    s.npcDamage = config.npcDamage;
    s.npcHp = config.npcHp;
    s.quickLoot = setup.QuickLoot ? 1 : 0;
    strncpy_s(s.difficulty, setup.Difficulty.GetText(), _TRUNCATE);
    bCString name("PC_Hero");
    auto& app = eCApplication::GetInstance();
    bCString const section("Game"), key("PlayerStart");
    auto read = [&](eCConfigFile const& file) {
        return originalString ? originalString(&file, section, key, name) : file.GetValue(section, key, name);
    };
    const bool found = app.GetLocalConfigFile().Contains(section, key)
        ? read(app.GetLocalConfigFile()) : read(app.GetConfigFile());
    if (!found) Log("options: PlayerStart absent/unreadable; native default PC_Hero used");
    // Overlong/invalid native values are explicit validation failures, not silently truncated host data.
    if (std::strlen(name.GetText()) >= sizeof(s.playerStart)) s.playerStart[0] = 0;
    else strcpy_s(s.playerStart, name.GetText());
    return s;
}

bool Adopt(policy::Snapshot const& host, uint32_t epoch)
{
    if (!installed || !policy::Valid(host)) {
        Log("options: invalid/unavailable host balancing snapshot; no options adopted");
        return false;
    }
    const auto local = Capture();
    const bool running = gCSession::GetInstance().IsGameRunning();
    const unsigned changed = policy::Differences(local, host);
    unsigned newValues = changed;
    policy::Snapshot original{};
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        const bool same = session.active && !policy::Differences(session.host, host) && session.epoch == epoch;
        if (same) return true;
        newValues = session.active ? policy::Differences(session.host, host) : changed;
        if (session.active && epoch && session.epoch == epoch && newValues) {
            Log("options: host changed balancing without a new world epoch; adoption refused");
            return false;
        }
        if (!session.Adopt(local, host, epoch)) return false;
        sessionActive.store(session.active, std::memory_order_release);
        original = session.original;
    }
    if (policy::NeedsEntry(newValues, running)) reentry = true;
    Logf("options: epoch=%08x option=alternative_balancing host=%u original=%u effective=%u source=host/getter",
         epoch, host.balancing, original.balancing, host.balancing);
    Logf("options: epoch=%08x option=alternative_ai host=%u original=%u effective=%u source=host/getter",
         epoch, host.ai, original.ai, host.ai);
    Logf("options: epoch=%08x option=min_hit_duration host=%d original=%d effective=%d source=host/getter",
         epoch, host.minHit, original.minHit, host.minHit);
    Logf("options: epoch=%08x option=xp_modifier host=%d original=%d effective=%d source=host/getter",
         epoch, host.xp, original.xp, host.xp);
    Logf("options: epoch=%08x option=PlayerStart host=%s original=%s effective=%s source=host/config-reader",
         epoch, host.playerStart, original.playerStart, host.playerStart);
    Logf("options: epoch=%08x option=friendly_fire host=%d original=%d effective=%d source=host/mod",
         epoch, host.friendlyFire, original.friendlyFire, host.friendlyFire);
    Logf("options: epoch=%08x option=npc_damage host=%.6g original=%.6g effective=%.6g source=host/mod",
         epoch, host.npcDamage, original.npcDamage, host.npcDamage);
    Logf("options: epoch=%08x option=npc_hp host=%.6g original=%.6g effective=%.6g source=host/mod",
         epoch, host.npcHp, original.npcHp, host.npcHp);
    Logf("options: epoch=%08x option=difficulty host=%s original=%s effective=%s source=local/kept NPC_claims=owner",
         epoch, host.difficulty, LocalSetup().Difficulty.GetText(), LocalSetup().Difficulty.GetText());
    Logf("options: epoch=%08x option=quick_loot host=%u original=%u effective=%u source=local/kept",
         epoch, host.quickLoot, LocalSetup().QuickLoot, LocalSetup().QuickLoot);
    if (newValues) Summary(changed, host);
    return true;
}

void Restore()
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (session.active) Log("options: restored local readers source=local disk_writes=0");
    session.Restore();
    sessionActive.store(false, std::memory_order_release);
    hint.clear();
    reentry = false;
}

void ApplyMod(Config& config)
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!session.active) return;
    policy::ApplyMod(config, session.host);
}

wchar_t const* Hint()
{
    thread_local std::wstring copy;
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        if (hint.empty()) return nullptr;
        copy = hint;
    }
    return copy.c_str();
}
bool WorldAnnounced()
{
    if (!reentry) return true;
    if (persist::ClientTransitionPending()) {
        reentry = false; // The existing epoch handler already queued this world replacement.
        return true;
    }
    const auto result = persist::EnterHostWorld("host-balancing");
    if (result == persist::EnterResult::Unavailable || result == persist::EnterResult::NotClient) {
        Log("options: host balancing needs world re-entry but the client template is unavailable; restart required");
        return false;
    }
    reentry = false;
    return true;
}
NpcJudgement::NpcJudgement(bool hostPresented) : entered(sessionActive.load(std::memory_order_acquire) && hostPresented)
{
    if (entered) ++hostNpcJudgement;
}
NpcJudgement::~NpcJudgement() { if (entered) --hostNpcJudgement; }
}
