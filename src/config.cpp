// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include "log.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace g3t {
namespace {
Config config;

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

Config const& GetConfig() { return config; }

Config const& LoadConfig()
{
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&LoadConfig), &self) ||
        !GetModuleFileNameA(self, path, MAX_PATH))
        return config;
    for (int part = 0; part != 2; ++part) {
        char* separator = std::strrchr(path, '\\');
        if (separator) *separator = '\0';
    }
    strcat_s(path, "\\g3together.ini");
    FILE* file = nullptr;
    if (fopen_s(&file, path, "r") || !file) {
        Log("config: g3together.ini not found, using defaults (host)");
        return config;
    }
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char* hash = std::strchr(line, '#');
        if (hash) *hash = '\0';
        char* equals = std::strchr(line, '=');
        if (!equals) continue;
        *equals = '\0';
        char key[64] = {}, value[192] = {};
        strncpy_s(key, line, _TRUNCATE);
        strncpy_s(value, equals + 1, _TRUNCATE);
        Trim(key);
        Trim(value);
        if (!_stricmp(key, "role")) config.host = _stricmp(value, "client") != 0;
        else if (!_stricmp(key, "session_id")) {
            config.sessionIdValid = ValidSessionId(value);
            if (config.sessionIdValid) strcpy_s(config.sessionId, value);
            else {
                config.sessionId[0] = '\0';
                Log("config: invalid session_id; use 1-63 ASCII letters, digits, '_' or '-'; networking will not start");
            }
        }
        else if (!_stricmp(key, "address") || !_stricmp(key, "port"))
            Logf("config: %s is no longer used; connections require Steam", key);
        else if (!_stricmp(key, "name")) Log("config: name is taken from Steam; the INI name is ignored");
        else if (!_stricmp(key, "transport")) {
            if (_stricmp(value, "steam")) Logf("config: transport '%s' is no longer supported; connections require Steam", value);
        }
        else if (!_stricmp(key, "steam_lobby")) strncpy_s(config.steamLobby, value, _TRUNCATE);
        else if (!_stricmp(key, "steam_host")) strncpy_s(config.steamHost, value, _TRUNCATE);
        else if (!_stricmp(key, "language")) {
            if (!_stricmp(value, "de")) strcpy_s(config.language, "de");
            else if (!_stricmp(value, "pl")) strcpy_s(config.language, "pl");
            else if (!_stricmp(value, "auto")) strcpy_s(config.language, "auto");
            else {
                strcpy_s(config.language, "en");
                if (_stricmp(value, "en")) Logf("config: unknown language '%s', using en", value);
            }
        }
        else if (!_stricmp(key, "puppet_template")) strncpy_s(config.puppetTemplate, value, _TRUNCATE);
        else if (!_stricmp(key, "look")) strncpy_s(config.look, value, _TRUNCATE);
        else if (!_stricmp(key, "partner_key")) strncpy_s(config.partnerKey, value, _TRUNCATE);
        else if (!_stricmp(key, "qa_resendguard")) config.qaResendGuardOff = !_stricmp(value, "off");
        else if (!_stricmp(key, "qa_hosthint")) strncpy_s(config.qaHostHint, value, _TRUNCATE);
        else if (!_stricmp(key, "trace")) strncpy_s(config.trace, value, _TRUNCATE);
        else if (!_stricmp(key, "reputation")) strncpy_s(config.reputation, value, _TRUNCATE);
        else if (!_stricmp(key, "host_sim")) {
            config.hostSimElastic = false;
            config.hostSimSpheres = true;
            if (_stricmp(value, "spheres"))
                Logf("config: host_sim=%s ignored: host authority requires per-player simulation regions", value);
        }
        else if (!_stricmp(key, "puppet_lead")) {
            config.puppetLead = !_stricmp(value, "auto");
            if (!config.puppetLead && _stricmp(value, "off")) Logf("config: unknown puppet_lead '%s', using off", value);
        }
        else if (!_stricmp(key, "motion_seek")) {
            config.motionSeek = _stricmp(value, "off") != 0;
            if (config.motionSeek && _stricmp(value, "on")) Logf("config: unknown motion_seek '%s', using on", value);
        }
        else if (!_stricmp(key, "movement_smoothing")) {
            config.movementSmoothing = _stricmp(value, "off") != 0;
            if (config.movementSmoothing && _stricmp(value, "on")) Logf("config: unknown movement_smoothing '%s', using on", value);
        }
        else if (!_stricmp(key, "send_rate")) {
            const int hz = std::atoi(value);
            if (hz >= 20 && hz <= 60) config.sendRate = hz;
            else Logf("config: send_rate %s out of 20-60; using %d", value, config.sendRate);
        }
        else if (!_stricmp(key, "listener_textbox")) {
            config.listenerTextbox = !_stricmp(value, "on");
            if (!config.listenerTextbox && _stricmp(value, "off")) Logf("config: unknown listener_textbox '%s', using off", value);
        }
        else if (!_stricmp(key, "friendly_fire")) {
            const int percent = std::atoi(value);
            if (percent >= 0 && percent <= 100) config.friendlyFire = percent;
            else Logf("config: friendly_fire %s out of 0-100; using %d", value, config.friendlyFire);
        }
        else if (!_stricmp(key, "npc_damage") || !_stricmp(key, "npc_hp")) {
            const float factor = static_cast<float>(std::atof(value));
            float& target = !_stricmp(key, "npc_damage") ? config.npcDamage : config.npcHp;
            if (factor >= 0.1f && factor <= 10.0f) target = factor;
            else Logf("config: %s %s out of 0.1-10; using %.2f", key, value, target);
        }
        else if (!_stricmp(key, "host_roi")) {
            const float roi = static_cast<float>(std::atof(value));
            if (roi >= 1000.0f && roi <= 20000.0f) config.hostRoi = roi;
            else Logf("config: host_roi %s out of 1000-20000; using %.0f", value, config.hostRoi);
        }
    }
    fclose(file);
    Logf("config: role=%s name=Steam puppet_template=%s look=%s language=%s", config.host ? "host" : "client",
         config.puppetTemplate, *config.look ? config.look : "-", config.language);
    if (config.host) Logf("config: host_roi=%.0f host_sim=%s", config.hostRoi, config.hostSimElastic ? "elastic" : config.hostSimSpheres ? "spheres" : "off");
    Logf("config: puppet_lead=%s motion_seek=%s listener_textbox=%s friendly_fire=%d npc_damage=%.2f npc_hp=%.2f",
         config.puppetLead ? "auto" : "off", config.motionSeek ? "on" : "off", config.listenerTextbox ? "on" : "off", config.friendlyFire, config.npcDamage, config.npcHp);
    Logf("config: movement_smoothing=%s (100 ms replica display buffer; local controls remain native)",
         config.movementSmoothing ? "on" : "off");
    Logf("config: transport=steam session_id=%s steam_lobby=%s steam_host=%s", config.sessionIdValid ? config.sessionId : "(invalid)", *config.steamLobby ? config.steamLobby : "-",
         *config.steamHost ? config.steamHost : "-");
    Logf("config: send_rate=%d Hz (player states and every NPC that changed; unchanged NPCs every 100 ms)", config.sendRate);
    return config;
}
}
