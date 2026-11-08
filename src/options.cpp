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
#include <cstdio>
#include <string>
#include <mutex>

namespace g3t::options {
namespace {
policy::Session session;
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

void Summary(unsigned changed)
{
    hint.clear();
    if (!changed) return;
    auto const& s = session.host;
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
    hint = text;
    hud::Message(hud::Good, hint.c_str());
    char utf8[2400] = {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
    Logf("options: %s", utf8);
}
}

bool Install()
{
    if (installed) return true;
    auto* engine = reinterpret_cast<unsigned char*>(GetModuleHandleA("Engine.dll"));
    void* game = GetModuleHandleA("Game.dll");
    auto* entry = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(reinterpret_cast<HMODULE>(engine),
        "?GetEngineSetup@eCApplication@@QBEABUeSSetupEngine@@XZ")) : nullptr;
    const unsigned char getter[] = {0x8d, 0x81, 0x98, 0, 0, 0, 0xc3};
    if (!game || !entry || std::memcmp(entry, getter, sizeof(getter))) {
        Log("options: native setup getter unavailable/changed; host balancing delegation cannot start");
        return false;
    }
    gameBase = reinterpret_cast<uintptr_t>(game);
    // Verify the one getter-based writer before allowing an overlay there.
    const unsigned char writer[] = {0x0f, 0xb6, 0x80, 0xea, 0x01, 0, 0, 0x50};
    if (std::memcmp(reinterpret_cast<void*>(gameBase + 0x90b0e), writer, sizeof(writer))) {
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
    const bool same = session.active && !policy::Differences(session.host, host) && session.epoch == epoch;
    if (same) return true;
    const unsigned changed = policy::Differences(local, host);
    const unsigned newValues = session.active ? policy::Differences(session.host, host) : changed;
    if (session.active && epoch && session.epoch == epoch && newValues) {
        Log("options: host changed balancing without a new world epoch; adoption refused");
        return false;
    }
    if (policy::NeedsEntry(newValues, gCSession::GetInstance().IsGameRunning())) reentry = true;
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        if (!session.Adopt(local, host, epoch)) return false;
    }
    auto const& original = session.original;
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
    if (newValues) Summary(changed);
    return true;
}

void Restore()
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (session.active) Log("options: restored local readers source=local disk_writes=0");
    session.Restore();
    hint.clear();
    reentry = false;
}

void ApplyMod(Config& config)
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!session.active) return;
    policy::ApplyMod(config, session.host);
}

wchar_t const* Hint() { return hint.empty() ? nullptr : hint.c_str(); }
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
NpcJudgement::NpcJudgement(bool hostPresented) : entered(session.active && hostPresented)
{
    if (entered) ++hostNpcJudgement;
}
NpcJudgement::~NpcJudgement() { if (entered) --hostNpcJudgement; }
}
