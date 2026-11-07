// SPDX-License-Identifier: GPL-3.0-or-later
#include "movement.h"
#include "movement_policy.h"
#include "ai.h"
#include "config.h"
#include "downed.h"
#include "guards.h"
#include "log.h"
#include "net.h"
#include "partner.h"
#include "persist.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

namespace g3t::movement {
namespace {
long long NowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Sample {
    uintptr_t body = 0, active = 0, focus = 0;
    float position[3] = {}, velocity[3] = {};
    int ai = -1, ani = -1, mode = -1;
    unsigned input = 0;
    bool moving = false;
};
struct Call {
    char kind[24] = {}, value[64] = {};
    uintptr_t caller = 0;
    long long at = 0;
    char const* stage = "engine";
    unsigned count = 0;
    void* stack[4] = {};
    unsigned stackCount = 0;
};
std::array<Call, 16> calls{};
unsigned nextCall = 0, callCount = 0, keyCalls = 0, keyBlocked = 0, contextClears = 0;
int lastKey = 0;
long long lastKeyAt = 0;
bool activeFrame = false, hooksChecked = false;
char const* phase = "engine";
Sample previous, boundary;
long long frameAt = 0, sampledAt = 0, previousAt = 0, previousSampleAt = 0, previousEnd = 0, reportAt = 0, anomalyAt = 0;
uint64_t gapMax = 0, engineMax = 0, admissionMax = 0, moduleMax = 0, stageMax = 0, diagnosticMax = 0;
std::array<unsigned, 5> gaps{};
char const* slowest = "-";
float speed = 0.0f;
StopWatch stop;

Sample Capture()
{
    Sample s;
    if (ai::ShuttingDown() || persist::NativeLoading() || !gCSession::GetInstance().IsGameRunning()) return s;
    eCEntity* body = remote::ControlledBody();
    if (!body) return s;
    s.body = reinterpret_cast<uintptr_t>(body);
    const auto& p = body->GetWorldPosition();
    s.position[0] = p.GetX(); s.position[1] = p.GetY(); s.position[2] = p.GetZ();
    auto* movement = static_cast<gCCharacterMovement_PS*>(body->GetPropertySet(eEPropertySetType_CharacterMovement));
    if (movement) {
        const auto v = movement->GetLocalVelocity();
        s.velocity[0] = v.GetX(); s.velocity[1] = v.GetY(); s.velocity[2] = v.GetZ();
        s.moving = movement->IsInMovingState();
        s.mode = static_cast<int>(movement->GetMovementMode());
    }
    if (body->GetPropertySet(eEPropertySetType_ScriptRoutine)) {
        Entity player(body);
        s.ai = player.Routine.AIMode; s.ani = player.Routine.AniState;
    }
    auto& session = gCSession::GetInstance();
    auto& guiModule = CFFGUIModule::GetInstance();
    s.active = reinterpret_cast<uintptr_t>(guiModule.GetActiveWnd());
    s.focus = reinterpret_cast<uintptr_t>(guiModule.GetFocusWnd());
    s.input = (session.CanPlayerMove() ? 1u : 0u) | (session.IsInputEnabled() ? 2u : 0u) |
        (eCApplication::GetInstance().IsInputDeviceEnabled() ? 4u : 0u) | (guiModule.IsInputEnabled() ? 8u : 0u);
    return s;
}

unsigned PhysicalKeys()
{
    unsigned mask = 0;
    const int keys[] = {'W', 'A', 'S', 'D', VK_UP, VK_LEFT, VK_DOWN, VK_RIGHT};
    for (unsigned i = 0; i < 8; ++i) if (GetAsyncKeyState(keys[i]) & 0x8000) mask |= 1u << i;
    return mask;
}

void Caller(uintptr_t address, char* text, size_t size)
{
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(address), &module) && GetModuleFileNameA(module, path, MAX_PATH)) {
        char const* name = std::strrchr(path, '\\');
        Format(text, size, "%s+0x%x", name ? name + 1 : path,
               static_cast<unsigned>(address - reinterpret_cast<uintptr_t>(module)));
    } else Format(text, size, "%p", reinterpret_cast<void*>(address));
}

void Trail(long long now)
{
    const unsigned count = std::min(callCount, 4u);
    for (unsigned i = callCount - count; i < callCount; ++i) {
        const auto& call = calls[(nextCall + calls.size() - callCount + i) % calls.size()];
        if (now - call.at > 2000000) continue;
        char caller[MAX_PATH + 20];
        Caller(call.caller, caller, sizeof(caller));
        char stack[320] = {};
        size_t used = 0;
        for (unsigned frame = 0; frame < call.stackCount; ++frame) {
            char address[MAX_PATH + 20];
            Caller(reinterpret_cast<uintptr_t>(call.stack[frame]), address, sizeof(address));
            used += Format(stack + used, sizeof(stack) - used, "%s%s", frame ? "<" : "", address);
        }
        Logf("movement-call: age_ms=%lld kind=%s value=%s stage=%s caller=%s repeats=%u stack=%s",
             (now - call.at) / 1000, call.kind, call.value, call.stage, caller, call.count, stack);
    }
}

void Event(char const* kind, char const* stage, Sample const& before, Sample const& after, long long now)
{
    if (std::strcmp(kind, "held-key-stop") && anomalyAt && now - anomalyAt < 1000000) return;
    anomalyAt = now;
    eCEntity* body = remote::ControlledBody();
    Entity player(body);
    const bool routine = body && body->GetPropertySet(eEPropertySetType_ScriptRoutine);
    Logf("movement: event=%s stage=%s frame_gap_ms=%.2f speed_cm_s=%.1f keys_wasd_arrows=%02x foreground=%d "
         "input=%x->%x gui_active=%p->%p gui_focus=%p->%p moving=%d->%d mode=%d->%d ai=%d->%d ani=%d->%d "
         "pos=%.1f,%.1f,%.1f->%.1f,%.1f,%.1f vel=%.1f,%.1f,%.1f task=%.40s state=%.40s "
         "held=%d terminal=%d key_calls=%u key_blocked=%u last_key=%d key_age_ms=%lld",
         kind, stage, previousAt ? (frameAt - previousAt) / 1000.0 : 0.0, speed,
         PhysicalKeys(), partner::Focused(), before.input, after.input,
         reinterpret_cast<void*>(before.active), reinterpret_cast<void*>(after.active),
         reinterpret_cast<void*>(before.focus), reinterpret_cast<void*>(after.focus),
         before.moving, after.moving, before.mode, after.mode, before.ai, after.ai, before.ani, after.ani,
         before.position[0], before.position[1], before.position[2], after.position[0], after.position[1], after.position[2],
         after.velocity[0], after.velocity[1], after.velocity[2],
         routine ? player.Routine.GetCurrentTask().GetText() : "-",
         routine ? player.Routine.GetCurrentState().GetText() : "-",
         downed::Held(), downed::Terminal(), keyCalls, keyBlocked, lastKey,
         lastKeyAt ? (now - lastKeyAt) / 1000 : -1);
    Trail(now);
}

void ObserveBoundary(char const* name)
{
    const Sample current = Capture();
    if (boundary.body && boundary.body == current.body) {
        const float dx = current.position[0] - boundary.position[0], dz = current.position[2] - boundary.position[2];
        const float beforeVelocity = boundary.velocity[0] * boundary.velocity[0] + boundary.velocity[2] * boundary.velocity[2];
        const float afterVelocity = current.velocity[0] * current.velocity[0] + current.velocity[2] * current.velocity[2];
        const bool input = current.input != boundary.input || current.active != boundary.active || current.focus != boundary.focus;
        if (dx * dx + dz * dz >= 900.0f) Event("position-write", name, boundary, current, NowUs());
        else if (input) Event("input-change", name, boundary, current, NowUs());
        else if (beforeVelocity >= 1600.0f && afterVelocity <= 25.0f)
            Event("velocity-stop", name, boundary, current, NowUs());
        else if ((boundary.moving && !current.moving) || boundary.ai != current.ai || boundary.ani != current.ani)
            Event("state-change", name, boundary, current, NowUs());
    }
    boundary = current;
}

using StopFn = void(__fastcall*)(gCCharacterMovement_PS*, void*);
using FullStopFn = void(__fastcall*)(gCScriptRoutine_PS*, void*);
StopFn originalStop = nullptr;
FullStopFn originalFullStop = nullptr;

void __fastcall NativeStop(gCCharacterMovement_PS* self, void* edx)
{
    Touch(self ? self->GetEntity() : nullptr, "StopMovement", "-", _ReturnAddress());
    originalStop(self, edx);
}
void __fastcall NativeFullStop(gCScriptRoutine_PS* self, void* edx)
{
    Touch(self ? self->GetEntity() : nullptr, "AIFullStop", "-", _ReturnAddress());
    originalFullStop(self, edx);
}
void Install()
{
    if (hooksChecked) return;
    auto* game = reinterpret_cast<unsigned char*>(GetModuleHandleA("Game.dll"));
    if (!game) return;
    hooksChecked = true;
    const unsigned char stopBytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    const unsigned char fullBytes[] = {0x8d, 0x41, 0x64, 0x8b, 0x48, 0x74};
    auto* stopEntry = reinterpret_cast<unsigned char*>(GetProcAddress(reinterpret_cast<HMODULE>(game),
        "?StopMovement@gCCharacterMovement_PS@@QAEXXZ"));
    auto* fullEntry = reinterpret_cast<unsigned char*>(GetProcAddress(reinterpret_cast<HMODULE>(game),
        "?AIFullStop@gCScriptRoutine_PS@@QAEXXZ"));
    if (stopEntry == game + 0xeb660 && !std::memcmp(stopEntry, stopBytes, sizeof(stopBytes)))
        originalStop = reinterpret_cast<StopFn>(ai::JumpTo(stopEntry, reinterpret_cast<void*>(&NativeStop), sizeof(stopBytes)));
    if (fullEntry == game + 0x164430 && !std::memcmp(fullEntry, fullBytes, sizeof(fullBytes)))
        originalFullStop = reinterpret_cast<FullStopFn>(ai::JumpTo(fullEntry, reinterpret_cast<void*>(&NativeFullStop), sizeof(fullBytes)));
    Logf("movement: native observers stop=%d full_stop=%d (read-only; missing observer leaves attribution incomplete)",
         originalStop != nullptr, originalFullStop != nullptr);
}
}

void Touch(eCEntity* entity, char const* kind, char const* value, void* caller)
{
    if (GetConfig().host || ai::ShuttingDown() || persist::NativeLoading() || !guards::IsLocalPlayer(entity) ||
        !gCSession::GetInstance().IsGameRunning()) return;
    const long long now = NowUs();
    if (!std::strcmp(kind, "local-context")) ++contextClears;
    if (callCount) {
        auto& last = calls[(nextCall + calls.size() - 1) % calls.size()];
        if (!std::strcmp(last.kind, kind) && !std::strcmp(last.value, value) &&
            last.caller == reinterpret_cast<uintptr_t>(caller) && now - last.at < 100000) {
            last.at = now; ++last.count; return;
        }
    }
    auto& call = calls[nextCall];
    strcpy_s(call.kind, kind); strncpy_s(call.value, value, _TRUNCATE);
    call.caller = reinterpret_cast<uintptr_t>(caller); call.at = now; call.stage = phase; call.count = 1;
    call.stackCount = caller ? CaptureStackBackTrace(2, 4, call.stack, nullptr) : 0;
    nextCall = (nextCall + 1) % calls.size();
    callCount = std::min<unsigned>(callCount + 1, static_cast<unsigned>(calls.size()));
}

void KeyDispatch(int key, bool blocked)
{
    if (GetConfig().host || ai::ShuttingDown() || persist::NativeLoading()) return;
    ++keyCalls; if (blocked) ++keyBlocked;
    lastKey = key; lastKeyAt = NowUs();
}

long long EntryTime() { return NowUs(); }

void Begin(long long entered)
{
    if (GetConfig().host) return;
    if (ai::ShuttingDown() || persist::NativeLoading() ||
        !gCSession::GetInstance().IsGameRunning()) { OnWorldChange(); return; }
    Install();
    const long long now = NowUs();
    frameAt = entered; sampledAt = now; activeFrame = true; phase = "module";
    admissionMax = std::max(admissionMax, static_cast<uint64_t>(now - entered));
    const Sample current = Capture();
    if (previous.body && current.body == previous.body && previousAt && now > previousAt) {
        const auto gap = static_cast<uint64_t>(entered - previousAt);
        ++gaps[GapBucket(gap)]; gapMax = std::max(gapMax, gap);
        if (previousEnd) engineMax = std::max(engineMax, static_cast<uint64_t>(entered - previousEnd));
        const float dx = current.position[0] - previous.position[0], dz = current.position[2] - previous.position[2];
        speed = std::sqrt(dx * dx + dz * dz) * 1000000.0f / static_cast<float>(now - previousSampleAt);
        const bool eligible = current.input == 15 && partner::Focused() && !gCSession::GetInstance().IsPaused() &&
            !downed::Held() && !downed::Terminal() && gap < 250000;
        if (stop.Observe(now, speed, PhysicalKeys() != 0, eligible))
            Event("held-key-stop", "engine", previous, current, now);
        else if (gap >= 100000) Event("frame-gap", "engine", previous, current, now);
        else if (previous.input != current.input || previous.active != current.active || previous.focus != current.focus)
            Event("input-change", "engine", previous, current, now);
    } else {
        stop.Reset(); speed = 0.0f;
    }
    previous = current; boundary = current;
}

Stage::Stage(char const* name) : before(phase), started(activeFrame ? NowUs() : 0)
{
    if (started) phase = name;
}
Stage::~Stage()
{
    if (!started || !activeFrame) return;
    const auto elapsed = static_cast<uint64_t>(NowUs() - started);
    if (elapsed > stageMax) { stageMax = elapsed; slowest = phase; }
    const long long observeAt = NowUs();
    ObserveBoundary(phase);
    diagnosticMax = std::max(diagnosticMax, static_cast<uint64_t>(NowUs() - observeAt));
    phase = before;
}

void End()
{
    if (!activeFrame) return;
    const long long now = NowUs();
    moduleMax = std::max(moduleMax, static_cast<uint64_t>(now - frameAt));
    if (!reportAt) reportAt = now;
    if (now - reportAt >= 1000000) {
        Logf("movement-frame: role=client bins_le16_33_66_100_over100=%u,%u,%u,%u,%u gap_max_ms=%.2f "
             "engine_max_ms=%.2f admission_max_ms=%.2f module_max_ms=%.2f stage=%s stage_max_ms=%.2f diagnostic_max_ms=%.2f "
             "speed_cm_s=%.1f keys_wasd_arrows=%02x key_calls=%u key_blocked=%u context_clears=%u",
             gaps[0], gaps[1], gaps[2], gaps[3], gaps[4], gapMax / 1000.0, engineMax / 1000.0,
             admissionMax / 1000.0, moduleMax / 1000.0, slowest, stageMax / 1000.0, diagnosticMax / 1000.0,
             speed, PhysicalKeys(), keyCalls, keyBlocked, contextClears);
        gaps = {}; gapMax = engineMax = admissionMax = moduleMax = stageMax = diagnosticMax = 0;
        slowest = "-"; keyCalls = keyBlocked = contextClears = 0; reportAt = now;
    }
    previousAt = frameAt; previousSampleAt = sampledAt; previousEnd = NowUs();
    activeFrame = false; phase = "engine";
}

void OnWorldChange()
{
    activeFrame = false; phase = "engine"; previous = {}; boundary = {};
    previousAt = previousSampleAt = previousEnd = reportAt = anomalyAt = 0; stop.Reset();
    calls = {}; callCount = nextCall = keyCalls = keyBlocked = contextClears = 0; lastKeyAt = 0;
    gaps = {}; gapMax = engineMax = admissionMax = moduleMax = stageMax = diagnosticMax = 0; slowest = "-";
}
}
