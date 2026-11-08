// SPDX-License-Identifier: GPL-3.0-or-later
#include "parity.h"

#include "config.h"
#include "log.h"
#include "options_policy.h"

#include <Engine.h>
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace g3t::parity {
namespace {
uint64_t items[ItemCount] = {};

struct Fnv
{
    uint64_t value = 1469598103934665603ull;
    void Add(void const* data, size_t size)
    {
        auto const* bytes = static_cast<unsigned char const*>(data);
        for (size_t i = 0; i != size; ++i) { value ^= bytes[i]; value *= 1099511628211ull; }
    }
    void Add(char const* text) { Add(text, std::strlen(text) + 1); }
    template <class T> void AddValue(T const& v) { Add(&v, sizeof(v)); }
};

std::string GameDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* sep = std::strrchr(path, '\\')) *sep = '\0';
    return path;
}

std::string Lower(std::string text)
{
    for (char& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

// The file's bytes; a missing file adds a marker instead, so "missing" differs from "empty".
void AddFile(Fnv& fnv, std::string const& path)
{
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) { fnv.Add("<missing>"); return; }
    static std::vector<unsigned char> buffer(1 << 20);
    DWORD read = 0;
    while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read) fnv.Add(buffer.data(), read);
    CloseHandle(file);
}

struct Found { std::string name; uint64_t size; };

std::vector<Found> FilesIn(std::string const& folder, char const* pattern)
{
    std::vector<Found> out;
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA((folder + "\\" + pattern).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back({Lower(data.cFileName), (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow});
    } while (FindNextFileA(find, &data));
    FindClose(find);
    std::sort(out.begin(), out.end(), [](Found const& a, Found const& b) { return a.name < b.name; });
    return out;
}

// Archives that hold the rules: item and NPC templates (alternative balancing swaps these), dialogs, quests.
bool IsRulesArchive(std::string const& name)
{
    for (char const* prefix : {"templates.", "infos.", "quests."})
        if (name.rfind(prefix, 0) == 0) return true;
    return false;
}

char optionsText[128] = "";

uint64_t HashOptions(std::string const& dir)
{
    Fnv fnv;
    fnv.AddValue(options::policy::Capability);
    eSSetupEngine const& setup = eCApplication::GetInstance().GetEngineSetup();
    char const* difficulty = setup.Difficulty.GetText();
    Logf("PARITY options difficulty=%s alternative_balancing=%d alternative_ai=%d min_hit_duration=%d xp_modifier=%d quick_loot=%d "
         "friendly_fire=%d npc_damage=%.2f npc_hp=%.2f",
         difficulty ? difficulty : "", setup.AlternativeBalancing, setup.AlternativeAI, setup.MinHitDuration, setup.XPModifier,
         setup.QuickLoot, GetConfig().friendlyFire, GetConfig().npcDamage, GetConfig().npcHp);
    strcpy_s(optionsText, "delegation capability / unsupported [Game] keys");
    static char section[8192];
    const DWORD n = GetPrivateProfileSectionA("Game", section, sizeof(section), (dir + "\\Ini\\ge3.ini").c_str());
    std::vector<std::string> lines;
    if (n == sizeof(section) - 2) {
        fnv.Add("<Game section truncated>");
        Log("PARITY [Game] section too large; install identity includes an invalid-section marker");
    }
    unsigned shared = 0, local = 0;
    for (char const* line = section; line < section + n && *line; line += std::strlen(line) + 1) {
        std::string text(line);
        const auto equals = text.find('=');
        const std::string key = options::policy::LowerTrim(text.substr(0, equals));
        const auto kind = options::policy::Classify(key);
        if (kind == options::policy::Class::Host) { ++shared; continue; }
        if (kind == options::policy::Class::Local) { ++local; continue; }
        const std::string value = equals == std::string::npos ? "" : options::policy::LowerTrim(text.substr(equals + 1));
        lines.push_back(key + "=" + value);
        Logf("PARITY unsupported [Game] key=%s value=%s (identity; not delegated)", key.c_str(), value.c_str());
    }
    std::sort(lines.begin(), lines.end());
    for (auto const& line : lines) fnv.Add(line.c_str());
    Logf("PARITY ini_game_lines=%u shared=%u local=%u identity=%u difficulty=per-player balancing=host",
         shared + local + static_cast<unsigned>(lines.size()), shared, local, static_cast<unsigned>(lines.size()));
    if (!n) Log("PARITY no [Game] entries; using native effective defaults, not refusing local preferences");
    return fnv.value;
}
}

char const* ItemName(int item)
{
    static char const* const names[ItemCount] = {"Game.dll", "Script.dll", "Engine.dll", "scripts folder", "rules archives",
                                                 "other archives", "options"};
    return item >= 0 && item < ItemCount ? names[item] : "?";
}

uint64_t const* Local() { return items; }
char const* OptionsText() { return optionsText; }

void Compute()
{
    const DWORD began = GetTickCount();
    const std::string dir = GameDir();
    char const* const dlls[] = {"Game.dll", "Script.dll", "Engine.dll"};
    for (int i = 0; i != 3; ++i) { Fnv fnv; AddFile(fnv, dir + "\\" + dlls[i]); items[GameDll + i] = fnv.value; }

    Fnv scripts;
    for (auto const& f : FilesIn(dir + "\\scripts", "*.dll")) { scripts.Add(f.name.c_str()); AddFile(scripts, dir + "\\scripts\\" + f.name); }
    items[ScriptsFolder] = scripts.value;

    Fnv rules, other;
    for (auto const& f : FilesIn(dir + "\\Data", "*")) {
        if (IsRulesArchive(f.name)) { rules.Add(f.name.c_str()); AddFile(rules, dir + "\\Data\\" + f.name); }
        else { other.Add(f.name.c_str()); AddFile(other, dir + "\\Data\\" + f.name); }
    }
    items[DataRules] = rules.value;
    items[DataOther] = other.value;
    items[Options] = HashOptions(dir);
    for (int i = 0; i != ItemCount; ++i) Logf("PARITY item=%d name=%s hash=%016llx", i, ItemName(i), items[i]);
    Logf("PARITY computed in %lu ms", GetTickCount() - began);
}
}
