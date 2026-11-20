// SPDX-License-Identifier: GPL-3.0-or-later
#include "partner.h"

#include "body.h"
#include "config.h"
#include "downed.h"
#include "hud.h"
#include "i18n.h"
#include "log.h"
#include "net.h"
#include "persist.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace g3t::partner {
// A key from the INI: F1..F24, PAGEDOWN, PAGEUP, HOME, END, INSERT, DELETE, or a single letter or digit.
int KeyFromName(char const* name, int fallback)
{
    if (!name || !*name) return fallback;
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        int n = 0;
        bool valid = true;
        for (char const* p = name + 1; *p; ++p) {
            if (*p < '0' || *p > '9') { valid = false; break; }
            n = n * 10 + (*p - '0');
            if (n > 24) { valid = false; break; }
        }
        if (valid && n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (!_stricmp(name, "PAGEDOWN")) return VK_NEXT;
    if (!_stricmp(name, "PAGEUP")) return VK_PRIOR;
    if (!_stricmp(name, "HOME")) return VK_HOME;
    if (!_stricmp(name, "END")) return VK_END;
    if (!_stricmp(name, "INSERT")) return VK_INSERT;
    if (!_stricmp(name, "DELETE")) return VK_DELETE;
    if (!_stricmp(name, "NUMPLUS")) return VK_ADD;
    if (!_stricmp(name, "NUMMINUS")) return VK_SUBTRACT;
    if (name[0] && !name[1]) {
        const char c = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    return fallback;
}

// The INI name of a key KeyFromName knows ("F7", "PAGEDOWN", "A"), for messages.
void KeyName(int code, char* out, size_t size)
{
    char const* named = code == VK_NEXT ? "PAGEDOWN" : code == VK_PRIOR ? "PAGEUP" : code == VK_HOME ? "HOME" : code == VK_END ? "END"
                        : code == VK_INSERT ? "INSERT" : code == VK_DELETE ? "DELETE" : code == VK_ADD ? "NUMPLUS"
                        : code == VK_SUBTRACT ? "NUMMINUS" : nullptr;
    if (named) strcpy_s(out, size, named);
    else if (code >= VK_F1 && code <= VK_F24) Format(out, size, "F%d", code - VK_F1 + 1);
    else if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) Format(out, size, "%c", code);
    else Format(out, size, "0x%02X", code);
}

// The game window of this process has the keyboard (two test copies on one PC: only the focused one reacts).
bool Focused()
{
    DWORD owner = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &owner);
    return owner == GetCurrentProcessId();
}

namespace {
constexpr uint32_t Cooldown = 5000;  // ms between jumps
uint32_t lastJump = 0;
bool hasJumped = false;
bool wasDown = false;
uint32_t lastWorldEpoch = 0, lastWelcomeSerial = 0;
unsigned jumps = 0, refused = 0;

int KeyCode() { return KeyFromName(GetConfig().partnerKey, VK_NEXT); }

// The other player: the host for a client, the first connected client in this world for the host.
int Partner()
{
    net::Remote const* remotes = net::Remotes();
    for (int id = 1; id < net::MaxPlayers; ++id) {
        if (id == net::LocalId() || !remotes[id].connected || !net::InWorld(static_cast<uint8_t>(id))) continue;
        if (!(remotes[id].latest.flags & 1) || remotes[id].latest.world != remote::CurrentWorld()) continue;
        return id;
    }
    return 0;
}
}

bool Jump(char const** reason)
{
    char const* unused = nullptr;
    if (!reason) reason = &unused;
    gCSession& session = gCSession::GetInstance();
    eCEntity* body = remote::ControlledBody();
    if (!body || !session.IsGameRunning() || persist::HeroPending() || persist::NativeLoading() ||
        !net::InWorld(net::LocalId())) {
        *reason = "no game";
        return false;
    }
    Entity self(body);
    if (self.IsDead() || self.IsDown() || downed::Held() || downed::Terminal() || downed::CombatUnavailable(body)) {
        *reason = "down";
        return false;
    }
    const int id = Partner();
    if (!id) { *reason = "no partner in this world"; return false; }
    net::PlayerState const& them = net::Remotes()[id].latest;
    for (float v : them.position) if (!std::isfinite(v)) { *reason = "no partner in this world"; return false; }
    // A partner lying down is a reason to jump (to help it up), not against it.
    const uint32_t now = net::Now();
    if (hasJumped && static_cast<int32_t>(now - lastJump) < static_cast<int32_t>(Cooldown)) {
        *reason = "too soon";
        return false;
    }
    float rot[4] = {them.rotation[0], them.rotation[1], them.rotation[2], them.rotation[3]};
    bool finiteRot = true;
    for (float v : rot) finiteRot = finiteRot && std::isfinite(v);
    float qlen = finiteRot ? std::sqrt(rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3]) : 0.0f;
    if (qlen > 1e-4f) {
        for (float& v : rot) v /= qlen;
    } else {
        rot[0] = rot[1] = rot[2] = 0.0f;
        rot[3] = 1.0f;
    }
    // 1.5 m behind the partner, along its facing (quaternion rotated forward vector), on its height.
    const float x = rot[0], y = rot[1], z = rot[2], w = rot[3];
    const float forward[3] = {2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y)};
    const float target[3] = {them.position[0] - forward[0] * 150.0f, them.position[1], them.position[2] - forward[2] * 150.0f};
    self.MoveTo(body::Pose(*body, target, rot));
    lastJump = now;
    hasJumped = true;
    ++jumps;
    Logf("partner: jumped to player %d (%.0f, %.0f, %.0f)", id, target[0], target[1], target[2]);
    *reason = "ok";
    return true;
}

void Tick()
{
    gCSession& session = gCSession::GetInstance();
    const uint32_t epoch = net::WorldEpoch();
    const uint32_t welcome = net::WelcomeSerial();
    if (!session.IsGameRunning() || epoch != lastWorldEpoch || welcome != lastWelcomeSerial) {
        lastJump = 0;
        hasJumped = false;
        wasDown = false;
        lastWorldEpoch = epoch;
        lastWelcomeSerial = welcome;
    }
    gCGUIManager* gui = session.GetGUIManager();
    if (!session.IsGameRunning() || session.IsPaused() || persist::HeroPending() ||
        persist::NativeLoading() || (gui && gui->IsMenuOpen()) || !Focused()) {
        wasDown = false;
        return;
    }
    const bool down = (GetAsyncKeyState(KeyCode()) & 0x8000) != 0;
    if (down && !wasDown) {
        char const* reason = "";
        if (Jump(&reason)) {
            hud::Message(hud::Good, i18n::Text(i18n::JumpedToPartner));
        } else {
            ++refused;
            if (!std::strcmp(reason, "no partner in this world")) hud::Message(hud::Warning, i18n::Text(i18n::NoPartnerInWorld));
            else if (!std::strcmp(reason, "down")) hud::Message(hud::Warning, i18n::Text(i18n::CannotJumpWhileDown));
            else if (!std::strcmp(reason, "too soon")) hud::Message(hud::Warning, i18n::Text(i18n::JumpCooldown));
        }
    }
    wasDown = down;
}

void JumpQa(LogFn log)
{
    char const* reason = "";
    const bool ok = Jump(&reason);
    char line[160];
    Format(line, "PARTNERJUMP ok=%d reason=%s jumps=%u refused=%u", ok ? 1 : 0, reason, jumps, refused);
    log(line);
}
}
