// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include "log.h"
#include "player_identity.h"
#include "options.h"
#include "symbols.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace g3t {
namespace {
Config config;
std::atomic<uint32_t> configRevision{1};

void Trim(char* text)
{
    char* start = text;
    while (*start == ' ' || *start == '\t') ++start;
    std::memmove(text, start, std::strlen(start) + 1);
    for (size_t n = std::strlen(text); n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' || text[n - 1] == '\n'); --n)
        text[n - 1] = '\0';
    const size_t n = std::strlen(text);
    if (n >= 2 && text[0] == '"' && text[n - 1] == '"') {
        std::memmove(text, text + 1, n - 2);
        text[n - 2] = '\0';
    }
}
}

Config const& GetLocalConfig() { return config; }
Config const& GetConfig()
{
    thread_local Config base, effective;
    thread_local uint32_t seenRevision = 0;
    const uint32_t rev = configRevision.load(std::memory_order_acquire);
    if (seenRevision != rev) {
        base = config;
        effective = base;
        seenRevision = rev;
    }
    effective.friendlyFire = base.friendlyFire;
    effective.npcDamage = base.npcDamage;
    effective.npcHp = base.npcHp;
    options::ApplyMod(effective);
    return effective;
}

Config const& LoadConfig()
{
    Config next{};
    auto publish = [&]() -> Config const& {
        config = std::move(next);
        configRevision.fetch_add(1, std::memory_order_release);
        return config;
    };
    char path[MAX_PATH] = {};
    if (!symbols::GameDir(path))
        return publish();
    strcat_s(path, "\\g3together.ini");
    FILE* file = nullptr;
    if (fopen_s(&file, path, "r") || !file) {
        Log("config: g3together.ini not found, using defaults (host)");
        return publish();
    }
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (!std::strchr(line, '\n') && std::strlen(line) == sizeof(line) - 1) {
            int nextChar = fgetc(file);
            if (nextChar != '\n' && nextChar != EOF) {
                while ((nextChar = fgetc(file)) != '\n' && nextChar != EOF) {}
                Log("config: line too long; ignoring setting");
                continue;
            }
        }
        char const* setting = line;
        while (*setting == ' ' || *setting == '\t') ++setting;
        const bool identityMapping = !_strnicmp(setting, "legacy_companion_owner", 22) &&
            (setting[22] == ' ' || setting[22] == '\t' || setting[22] == '=');
        char* hash = std::strchr(line, '#');
        if (hash && !identityMapping) *hash = '\0'; // Migration labels may themselves contain '#'.
        char* equals = std::strchr(line, '=');
        if (!equals) continue;
        *equals = '\0';
        char key[64] = {}, value[192] = {};
        strncpy_s(key, line, _TRUNCATE);
        strncpy_s(value, equals + 1, _TRUNCATE);
        Trim(key);
        Trim(value);
        if (!_stricmp(key, "ardea_remove_npcs") && std::strlen(equals + 1) >= sizeof(value)) {
            Log("config: ardea_remove_npcs is too long; keeping previous setting");
            continue;
        }
        if (!_stricmp(key, "role")) next.host = _stricmp(value, "client") != 0;
        else if (!_stricmp(key, "session_id")) {
            next.sessionIdValid = ValidSessionId(value);
            if (next.sessionIdValid) strcpy_s(next.sessionId, value);
            else {
                next.sessionId[0] = '\0';
                Log("config: invalid session_id; use 1-63 ASCII letters, digits, '_' or '-'; networking will not start");
            }
        }
        else if (!_stricmp(key, "address") || !_stricmp(key, "port"))
            Logf("config: %s is no longer used; connections require Steam", key);
        else if (!_stricmp(key, "name")) Log("config: name is taken from Steam; the INI name is ignored");
        else if (!_stricmp(key, "transport")) {
            if (_stricmp(value, "steam")) Logf("config: transport '%s' is no longer supported; connections require Steam", value);
        }
        else if (!_stricmp(key, "steam_lobby")) strncpy_s(next.steamLobby, value, _TRUNCATE);
        else if (!_stricmp(key, "connection")) {
            if (!_stricmp(value, "auto")) next.connectionAuto = true;
            else {
                next.connectionAuto = false;
                if (_stricmp(value, "invite")) Logf("config: unknown connection '%s', using invite", value);
            }
        }
        else if (!_stricmp(key, "steam_host")) strncpy_s(next.steamHost, value, _TRUNCATE);
        else if (!_stricmp(key, "legacy_companion_owner")) {
            const std::string mapping(value);
            const size_t space = mapping.find(' ');
            uint64_t account = 0;
            if (space == std::string::npos || !identity::SteamId(mapping.substr(0, space), account) ||
                !identity::Label(mapping.substr(space + 1)) || std::strlen(equals + 1) >= sizeof(value)) {
                Log("identity: invalid legacy_companion_owner; use <SteamID64> <old display name>");
            } else {
                auto [entry, inserted] = next.legacyCompanionOwners.emplace(mapping.substr(space + 1), account);
                if (!inserted && entry->second != account) {
                    entry->second = 0;
                    Log("identity: conflicting legacy_companion_owner mappings; migration refused");
                }
            }
        }
        else if (!_stricmp(key, "language")) {
            if (!_stricmp(value, "de")) strcpy_s(next.language, "de");
            else if (!_stricmp(value, "pl")) strcpy_s(next.language, "pl");
            else if (!_stricmp(value, "auto")) strcpy_s(next.language, "auto");
            else {
                strcpy_s(next.language, "en");
                if (_stricmp(value, "en")) Logf("config: unknown language '%s', using en", value);
            }
        }
        else if (!_stricmp(key, "puppet_template")) strncpy_s(next.puppetTemplate, value, _TRUNCATE);
        else if (!_stricmp(key, "look")) strncpy_s(next.look, value, _TRUNCATE);
        else if (!_stricmp(key, "partner_key")) strncpy_s(next.partnerKey, value, _TRUNCATE);
        else if (!_stricmp(key, "trace")) strncpy_s(next.trace, value, _TRUNCATE);
        else if (!_stricmp(key, "reputation")) strncpy_s(next.reputation, value, _TRUNCATE);
        else if (!_stricmp(key, "host_sim")) {
            next.hostSimElastic = false;
            next.hostSimSpheres = true;
            if (_stricmp(value, "spheres"))
                Logf("config: host_sim=%s ignored: host authority requires per-player simulation regions", value);
        }
        else if (!_stricmp(key, "puppet_lead")) {
            next.puppetLead = !_stricmp(value, "auto");
            if (!next.puppetLead && _stricmp(value, "off")) Logf("config: unknown puppet_lead '%s', using off", value);
        }
        else if (!_stricmp(key, "motion_seek")) {
            next.motionSeek = _stricmp(value, "off") != 0;
            if (next.motionSeek && _stricmp(value, "on")) Logf("config: unknown motion_seek '%s', using on", value);
        }
        else if (!_stricmp(key, "movement_smoothing")) {
            next.movementSmoothing = _stricmp(value, "off") != 0;
            if (next.movementSmoothing && _stricmp(value, "on")) Logf("config: unknown movement_smoothing '%s', using on", value);
        }
        else if (!_stricmp(key, "send_rate")) {
            const int hz = std::atoi(value);
            if (hz >= 20 && hz <= 60) next.sendRate = hz;
            else Logf("config: send_rate %s out of 20-60; using %d", value, next.sendRate);
        }
        else if (!_stricmp(key, "network_budget_kib")) {
            char* end = nullptr;
            const long kib = std::strtol(value, &end, 10);
            if (end != value && !*end && kib >= 64 && kib <= 4096)
                next.networkBudgetKib = static_cast<int>(kib);
            else Logf("config: network_budget_kib %s out of 64-4096; using %d", value, next.networkBudgetKib);
        }
        else if (!_stricmp(key, "client_autoload")) {
            next.clientAutoload = _stricmp(value, "off") != 0;
            if (next.clientAutoload && _stricmp(value, "on")) Logf("config: unknown client_autoload '%s', using on", value);
        }
        else if (!_stricmp(key, "listener_textbox")) {
            next.listenerTextbox = !_stricmp(value, "on");
            if (!next.listenerTextbox && _stricmp(value, "off")) Logf("config: unknown listener_textbox '%s', using off", value);
        }
        else if (!_stricmp(key, "friendly_fire")) {
            const int percent = std::atoi(value);
            if (percent >= 0 && percent <= 100) next.friendlyFire = percent;
            else Logf("config: friendly_fire %s out of 0-100; using %d", value, next.friendlyFire);
        }
        else if (!_stricmp(key, "npc_damage") || !_stricmp(key, "npc_hp")) {
            const float factor = static_cast<float>(std::atof(value));
            float& target = !_stricmp(key, "npc_damage") ? next.npcDamage : next.npcHp;
            if (factor >= 0.1f && factor <= 10.0f) target = factor;
            else Logf("config: %s %s out of 0.1-10; using %.2f", key, value, target);
        }
        else if (!_stricmp(key, "host_roi")) {
            const float roi = static_cast<float>(std::atof(value));
            if (roi >= 1000.0f && roi <= 20000.0f) next.hostRoi = roi;
            else Logf("config: host_roi %s out of 1000-20000; using %.0f", value, next.hostRoi);
        }
        else if (!_stricmp(key, "ardea_remove_npcs")) {
            if (!npcremoval::policy::ParseNames(value, next.ardeaRemoveNpcs))
                Logf("config: invalid ardea_remove_npcs '%s'; use Goblin,Ogre or an empty value; keeping previous setting", value);
        }
        else if (!_stricmp(key, "npc_authority_fallback")) {
            if (!_stricmp(value, "on")) next.npcAuthorityFallback = true;
            else if (!_stricmp(value, "off")) next.npcAuthorityFallback = false;
            else {
                next.npcAuthorityFallback = false;
                Logf("config: unknown npc_authority_fallback '%s'; restricted leases disabled", value);
            }
        }
    }
    fclose(file);
    Logf("config: role=%s name=Steam puppet_template=%s look=%s language=%s", next.host ? "host" : "client",
         next.puppetTemplate, *next.look ? next.look : "-", next.language);
    if (next.host) Logf("config: host_roi=%.0f host_sim=%s", next.hostRoi, next.hostSimElastic ? "elastic" : next.hostSimSpheres ? "spheres" : "off");
    else Logf("config: client_autoload=%s (enter the host's world automatically after a host load; Esc cancels the countdown)",
              next.clientAutoload ? "on" : "off");
    Logf("config: puppet_lead=%s motion_seek=%s listener_textbox=%s friendly_fire=%d npc_damage=%.2f npc_hp=%.2f",
         next.puppetLead ? "auto" : "off", next.motionSeek ? "on" : "off", next.listenerTextbox ? "on" : "off", next.friendlyFire, next.npcDamage, next.npcHp);
    Logf("config: movement_smoothing=%s (100 ms replica display buffer; local controls remain native)",
         next.movementSmoothing ? "on" : "off");
    Logf("config: transport=steam connection=%s session_id=%s steam_lobby=%s steam_host=%s",
         next.connectionAuto ? "auto" : "invite", next.sessionIdValid ? next.sessionId : "(invalid)",
         *next.steamLobby ? next.steamLobby : "-", *next.steamHost ? next.steamHost : "-");
    Logf("config: send_rate=%d Hz (compact player states; reliable gameplay immediate; NPC policy independent) network_budget_kib=%d per peer (bulk save pacing base; never caps NPC/gameplay traffic)",
         next.sendRate, next.networkBudgetKib);
    return publish();
}
}
