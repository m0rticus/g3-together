// SPDX-License-Identifier: GPL-3.0-or-later
#include "movement.h"
#include "movement_policy.h"
#include "position_policy.h"
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
    long long capturedAt = 0;
    float position[3] = {}, velocity[3] = {};
    int ai = -1, ani = -1, mode = -1;
    unsigned input = 0;
    bool moving = false;
    bool airborne = false;
};
struct Call {
    char kind[24] = {}, value[64] = {};
    uintptr_t caller = 0;
    long long at = 0;
    char const* stage = "engine";
    unsigned count = 0;
    unsigned frameSequence = 0, stageSequence = 0;
    void* stack[4] = {};
    unsigned stackCount = 0;
};
std::array<Call, 16> calls{};
unsigned nextCall = 0, callCount = 0, keyCalls = 0, keyBlocked = 0, contextClears = 0;
int lastKey = 0;
long long lastKeyAt = 0;
bool activeFrame = false, hooksChecked = false;
DWORD observerThread = 0;
unsigned nativeStops = 0, nativeFullStops = 0, nativeExits = 0, frameCalls = 0, stageCalls = 0;
char const* phase = "engine";
Sample previous, boundary;
long long boundaryAt = 0;
struct PositionWrite {
    Sample before, after;
    uintptr_t caller = 0;
    char const* kind = "-", *stage = "engine";
    uint64_t elapsedUs = 0;
};
std::array<PositionWrite, 8> positionWrites{};
unsigned positionWriteCount = 0, positionWriteDropped = 0;
unsigned transformDiscontinuities = 0, frameDiscontinuities = 0;
long long frameAt = 0, sampledAt = 0, previousAt = 0, previousSampleAt = 0, previousEnd = 0, reportAt = 0, anomalyAt = 0;
uint64_t gapMax = 0, engineMax = 0, admissionMax = 0, moduleMax = 0, stageMax = 0, diagnosticMax = 0;
std::array<unsigned, 5> gaps{};
char const* slowest = "-";
float speed = 0.0f;
StopWatch stop;

Sample Capture(eCEntity* observed = nullptr)
{
    Sample s;
    if (ai::ShuttingDown() || persist::NativeLoading() || !gCSession::GetInstance().IsGameRunning()) return s;
    eCEntity* body = observed ? observed : remote::ControlledBody();
    if (!body) return s;
    s.body = reinterpret_cast<uintptr_t>(body);
    s.capturedAt = NowUs();
    const auto& p = body->GetWorldPosition();
    s.position[0] = p.GetX(); s.position[1] = p.GetY(); s.position[2] = p.GetZ();
    auto* movement = static_cast<gCCharacterMovement_PS*>(body->GetPropertySet(eEPropertySetType_CharacterMovement));
    if (movement) {
        const auto v = movement->GetLocalVelocity();
        s.velocity[0] = v.GetX(); s.velocity[1] = v.GetY(); s.velocity[2] = v.GetZ();
        s.moving = movement->IsInMovingState();
        s.mode = static_cast<int>(movement->GetMovementMode());
        s.airborne = s.mode == 6 || s.mode == 13;
    }
    if (body->GetPropertySet(eEPropertySetType_ScriptRoutine)) {
        Entity player(body);
        s.ai = player.Routine.AIMode; s.ani = player.Routine.AniState;
    }
    if (observed) return s;
    auto& session = gCSession::GetInstance();
    auto& guiModule = CFFGUIModule::GetInstance();
    s.active = reinterpret_cast<uintptr_t>(guiModule.GetActiveWnd());
    s.focus = reinterpret_cast<uintptr_t>(guiModule.GetFocusWnd());
    s.input = (session.CanPlayerMove() ? 1u : 0u) | (session.IsInputEnabled() ? 2u : 0u) |
        (eCApplication::GetInstance().IsInputDeviceEnabled() ? 4u : 0u) | (guiModule.IsInputEnabled() ? 8u : 0u);
    return s;
}

bool Discontinuity(Sample const& before, Sample const& after, uint64_t elapsedUs)
{
    return before.body && before.body == after.body &&
        PositionDiscontinuity(before.position, after.position, before.velocity, after.velocity,
                              elapsedUs, before.airborne);
}

void Caller(uintptr_t address, char* text, size_t size);

void DrainPositionWrites()
{
    for (unsigned i = 0; i < positionWriteCount; ++i) {
        auto const& write = positionWrites[i];
        char caller[MAX_PATH + 20];
        Caller(write.caller, caller, sizeof(caller));
        Logf("movement-position: event=transform-discontinuity candidate=1 kind=%s stage=%s caller=%s dt_ms=%.2f "
             "pos=%.1f,%.1f,%.1f->%.1f,%.1f,%.1f vel=%.1f,%.1f,%.1f->%.1f,%.1f,%.1f "
             "mode=%d->%d ani=%d->%d airborne=%d held=%d terminal=%d",
             write.kind, write.stage, caller, write.elapsedUs / 1000.0,
             write.before.position[0], write.before.position[1], write.before.position[2],
             write.after.position[0], write.after.position[1], write.after.position[2],
             write.before.velocity[0], write.before.velocity[1], write.before.velocity[2],
             write.after.velocity[0], write.after.velocity[1], write.after.velocity[2],
             write.before.mode, write.after.mode, write.before.ani, write.after.ani,
             write.before.airborne, downed::Held(), downed::Terminal());
    }
    positionWriteCount = 0;
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

void Trail(long long now, char* text, size_t size)
{
    size_t length = 0;
    const unsigned count = callCount;
    for (unsigned i = callCount - count; i < callCount; ++i) {
        const auto& call = calls[(nextCall + calls.size() - callCount + i) % calls.size()];
        if (now - call.at > 2000000) continue;
        char caller[80];
        Caller(call.caller, caller, sizeof(caller));
        char stack[360] = {};
        size_t used = 0;
        for (unsigned frame = 0; frame < call.stackCount; ++frame) {
            char address[80];
            Caller(reinterpret_cast<uintptr_t>(call.stack[frame]), address, sizeof(address));
            used += Format(stack + used, sizeof(stack) - used, "%s%s", frame ? "<" : "", address);
        }
        length += Format(text + length, size - length,
            "%s{age_ms=%lld kind=%s value=%s stage=%s caller=%s repeats=%u frame_call=%u stage_call=%u stack=%s}",
            length ? ";" : "", (now - call.at) / 1000, call.kind, call.value, call.stage,
            caller, call.count, call.frameSequence, call.stageSequence, stack);
        if (length + 1 >= size) break;
    }
}

void Event(char const* kind, char const* stage, Sample const& before, Sample const& after, long long now)
{
    if (std::strcmp(kind, "held-key-stop") && anomalyAt && now - anomalyAt < 1000000) return;
    anomalyAt = now;
    eCEntity* body = remote::ControlledBody();
    Entity player(body);
    const bool routine = body && body->GetPropertySet(eEPropertySetType_ScriptRoutine);
    char trail[14000] = {};
    Trail(now, trail, sizeof(trail));
    Logf("movement: event=%s stage=%s frame_gap_ms=%.2f speed_cm_s=%.1f keys_wasd_arrows=%02x foreground=%d "
         "input=%x->%x gui_active=%p->%p gui_focus=%p->%p moving=%d->%d mode=%d->%d ai=%d->%d ani=%d->%d "
         "pos=%.1f,%.1f,%.1f->%.1f,%.1f,%.1f vel=%.1f,%.1f,%.1f task=%.40s state=%.40s "
         "held=%d terminal=%d key_calls=%u key_blocked=%u last_key=%d key_age_ms=%lld "
         "frame_native_stop=%u frame_native_full_stop=%u frame_native_exit=%u frame_calls=%u stage_calls=%u trail=%s",
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
         lastKeyAt ? (now - lastKeyAt) / 1000 : -1,
         nativeStops, nativeFullStops, nativeExits, frameCalls, stageCalls, trail);
}

void ObserveBoundary(char const* name)
{
    const long long now = NowUs();
    const Sample current = Capture();
    if (boundary.body && boundary.body == current.body) {
        const float beforeVelocity = boundary.velocity[0] * boundary.velocity[0] + boundary.velocity[2] * boundary.velocity[2];
        const float afterVelocity = current.velocity[0] * current.velocity[0] + current.velocity[2] * current.velocity[2];
        const bool input = current.input != boundary.input || current.active != boundary.active || current.focus != boundary.focus;
        if (boundaryAt && Discontinuity(boundary, current, static_cast<uint64_t>(now - boundaryAt))) {
            ++frameDiscontinuities;
            Event("position-discontinuity", name, boundary, current, now);
        }
        else if (input) Event("input-change", name, boundary, current, NowUs());
        else if (beforeVelocity >= 1600.0f && afterVelocity <= 25.0f)
            Event("velocity-stop", name, boundary, current, NowUs());
        else if ((boundary.moving && !current.moving) || boundary.ai != current.ai || boundary.ani != current.ani)
            Event("state-change", name, boundary, current, NowUs());
    }
    boundary = current;
    boundaryAt = now;
}

using StopFn = void(__fastcall*)(gCCharacterMovement_PS*, void*);
using FullStopFn = void(__fastcall*)(gCScriptRoutine_PS*, void*);
using ExitFn = void(__fastcall*)(eCEntity*, void*);
StopFn originalStop = nullptr;
FullStopFn originalFullStop = nullptr;
ExitFn originalExit = nullptr;
using MatrixFn = void(__fastcall*)(eCEntity*, void*, bCMatrix const&);
using PoseFn = void(__fastcall*)(eCRigidBody_PS*, void*, bCMatrix const&);
MatrixFn originalMatrix = nullptr;
PoseFn originalPose = nullptr;

Sample BeforeTransform(eCEntity* entity)
{
    if (GetConfig().host || GetCurrentThreadId() != observerThread ||
        ai::ShuttingDown() || persist::NativeLoading() ||
        !entity || !guards::IsLocalPlayer(entity)) return {};
    return Capture(entity);
}

void AfterTransform(Sample const& before, eCEntity* entity, char const* kind, void* caller)
{
    if (!before.body) return;
    const Sample after = Capture(entity);
    const uint64_t elapsed = before.capturedAt && after.capturedAt >= before.capturedAt
        ? static_cast<uint64_t>(after.capturedAt - before.capturedAt) : 0;
    if (!Discontinuity(before, after, elapsed)) return;
    ++transformDiscontinuities;
    if (positionWriteCount == positionWrites.size()) { ++positionWriteDropped; return; }
    positionWrites[positionWriteCount++] = {before, after, reinterpret_cast<uintptr_t>(caller), kind, phase, elapsed};
}

void __fastcall NativeMatrix(eCEntity* self, void* edx, bCMatrix const& matrix)
{
    const Sample before = BeforeTransform(self);
    originalMatrix(self, edx, matrix);
    AfterTransform(before, self, "SetToWorldMatrix", _ReturnAddress());
}

void __fastcall NativePose(eCRigidBody_PS* self, void* edx, bCMatrix const& matrix)
{
    eCEntity* entity = self ? self->GetEntity() : nullptr;
    const Sample before = BeforeTransform(entity);
    originalPose(self, edx, matrix);
    AfterTransform(before, entity, "SetPose", _ReturnAddress());
}

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
void __fastcall NativeExit(eCEntity* self, void* edx)
{
    Touch(self, "ExitProcessingRange", "-", _ReturnAddress());
    originalExit(self, edx);
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
    auto* engine = reinterpret_cast<unsigned char*>(GetModuleHandleA("Engine.dll"));
    const unsigned char exitBytes[] = {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x3c};
    auto* exitEntry = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(reinterpret_cast<HMODULE>(engine),
        "?OnExitProcessingRange@eCEntity@@MAEXXZ")) : nullptr;
    if (exitEntry && exitEntry == engine + 0x223af0 && !std::memcmp(exitEntry, exitBytes, sizeof(exitBytes)))
        originalExit = reinterpret_cast<ExitFn>(ai::JumpTo(exitEntry, reinterpret_cast<void*>(&NativeExit), sizeof(exitBytes)));
    const unsigned char poseBytes[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x11};
    auto* matrixEntry = reinterpret_cast<unsigned char*>(GetProcAddress(reinterpret_cast<HMODULE>(engine),
        "?SetToWorldMatrix@eCEntity@@QAEXABVbCMatrix@@@Z"));
    auto* poseEntry = reinterpret_cast<unsigned char*>(GetProcAddress(reinterpret_cast<HMODULE>(engine),
        "?SetPose@eCRigidBody_PS@@UAEXABVbCMatrix@@@Z"));
    if (engine && matrixEntry == engine + 0x227100 && !std::memcmp(matrixEntry, stopBytes, sizeof(stopBytes)))
        originalMatrix = reinterpret_cast<MatrixFn>(ai::JumpTo(matrixEntry, reinterpret_cast<void*>(&NativeMatrix), sizeof(stopBytes)));
    if (engine && poseEntry == engine + 0x177e10 && !std::memcmp(poseEntry, poseBytes, sizeof(poseBytes)))
        originalPose = reinterpret_cast<PoseFn>(ai::JumpTo(poseEntry, reinterpret_cast<void*>(&NativePose), sizeof(poseBytes)));
    Logf("movement-position: native observers matrix=%d rigid_pose=%d (read-only; missing observer leaves attribution incomplete)",
        originalMatrix != nullptr, originalPose != nullptr);
    Logf("movement: native observers stop=%d full_stop=%d processing_exit=%d (read-only; missing observer leaves attribution incomplete)",
        originalStop != nullptr, originalFullStop != nullptr, originalExit != nullptr);
}
}

void Touch(eCEntity* entity, char const* kind, char const* value, void* caller)
{
    if (GetConfig().host || GetCurrentThreadId() != observerThread ||
        ai::ShuttingDown() || persist::NativeLoading() || !guards::IsLocalPlayer(entity) ||
        !gCSession::GetInstance().IsGameRunning()) return;
    const long long now = NowUs();
    ++frameCalls; ++stageCalls;
    if (!std::strcmp(kind, "StopMovement")) ++nativeStops;
    if (!std::strcmp(kind, "AIFullStop")) ++nativeFullStops;
    if (!std::strcmp(kind, "ExitProcessingRange")) ++nativeExits;
    if (!std::strcmp(kind, "local-context")) ++contextClears;
    if (callCount) {
        auto& last = calls[(nextCall + calls.size() - 1) % calls.size()];
        if (!std::strcmp(last.kind, kind) && !std::strcmp(last.value, value) &&
            last.caller == reinterpret_cast<uintptr_t>(caller) && last.stage == phase && now - last.at < 100000) {
            last.at = now; ++last.count;
            last.frameSequence = frameCalls; last.stageSequence = stageCalls; return;
        }
    }
    auto& call = calls[nextCall];
    strcpy_s(call.kind, kind); strncpy_s(call.value, value, _TRUNCATE);
    call.caller = reinterpret_cast<uintptr_t>(caller); call.at = now; call.stage = phase; call.count = 1;
    call.frameSequence = frameCalls; call.stageSequence = stageCalls;
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
    observerThread = GetCurrentThreadId();
    if (ai::ShuttingDown() || persist::NativeLoading() ||
        !gCSession::GetInstance().IsGameRunning()) { OnWorldChange(); return; }
    nativeStops = nativeFullStops = nativeExits = frameCalls = stageCalls = 0;
    Install();
    const long long now = NowUs();
    frameAt = entered; sampledAt = now; activeFrame = true; phase = "module";
    if (now > entered) admissionMax = std::max(admissionMax, static_cast<uint64_t>(now - entered));
    const Sample current = Capture();
    if (previous.body && current.body == previous.body && previousAt && now > previousAt &&
        previousSampleAt && now > previousSampleAt) {
        const auto gap = entered > previousAt ? static_cast<uint64_t>(entered - previousAt) : 0ull;
        ++gaps[GapBucket(gap)]; gapMax = std::max(gapMax, gap);
        if (previousEnd && entered > previousEnd)
            engineMax = std::max(engineMax, static_cast<uint64_t>(entered - previousEnd));
        const float dx = current.position[0] - previous.position[0], dz = current.position[2] - previous.position[2];
        speed = std::sqrt(dx * dx + dz * dz) * 1000000.0f / static_cast<float>(now - previousSampleAt);
        const bool eligible = (current.input & 7u) == 7u && partner::Focused() && !gCSession::GetInstance().IsPaused() &&
            !downed::Held() && !downed::Terminal() && gap < 250000;
        if (stop.Observe(now, speed, PhysicalKeys() != 0, eligible))
            Event("held-key-stop", "engine", previous, current, now);
        else if (gap >= 100000) Event("frame-gap", "engine", previous, current, now);
        else if (Discontinuity(previous, current, static_cast<uint64_t>(now - previousSampleAt))) {
            ++frameDiscontinuities;
            Event("position-discontinuity", "engine", previous, current, now);
        }
        else if (previous.input != current.input || previous.active != current.active || previous.focus != current.focus)
            Event("input-change", "engine", previous, current, now);
    } else {
        stop.Reset(); speed = 0.0f;
    }
    previous = current; boundary = current;
    boundaryAt = now;
}

Stage::Stage(char const* name) : before(phase), started(activeFrame ? NowUs() : 0)
{
    if (started) {
        ObserveBoundary(before);
        phase = name;
        beforeCalls = stageCalls;
        stageCalls = 0;
    }
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
    stageCalls += beforeCalls;
}

void End()
{
    if (!activeFrame) return;
    const long long now = NowUs();
    DrainPositionWrites();
    moduleMax = std::max(moduleMax, static_cast<uint64_t>(now - frameAt));
    if (!reportAt) reportAt = now;
    if (now - reportAt >= 1000000) {
        Logf("movement-frame: role=client bins_le16_33_66_100_over100=%u,%u,%u,%u,%u gap_max_ms=%.2f "
             "engine_max_ms=%.2f admission_max_ms=%.2f module_max_ms=%.2f stage=%s stage_max_ms=%.2f diagnostic_max_ms=%.2f "
             "speed_cm_s=%.1f keys_wasd_arrows=%02x key_calls=%u key_blocked=%u context_clears=%u",
             gaps[0], gaps[1], gaps[2], gaps[3], gaps[4], gapMax / 1000.0, engineMax / 1000.0,
             admissionMax / 1000.0, moduleMax / 1000.0, slowest, stageMax / 1000.0, diagnosticMax / 1000.0,
             speed, PhysicalKeys(), keyCalls, keyBlocked, contextClears);
        Logf("movement-position: totals transform_discontinuities=%u frame_discontinuities=%u dropped=%u",
             transformDiscontinuities, frameDiscontinuities, positionWriteDropped);
        gaps = {}; gapMax = engineMax = admissionMax = moduleMax = stageMax = diagnosticMax = 0;
        slowest = "-"; keyCalls = keyBlocked = contextClears = 0; reportAt = now;
    }
    previousAt = frameAt; previousSampleAt = sampledAt; previousEnd = NowUs();
    activeFrame = false; phase = "engine";
}

void OnWorldChange()
{
    activeFrame = false; phase = "engine"; previous = {}; boundary = {};
    nativeStops = nativeFullStops = nativeExits = frameCalls = stageCalls = 0;
    boundaryAt = 0; positionWriteCount = positionWriteDropped = 0;
    transformDiscontinuities = frameDiscontinuities = 0;
    previousAt = previousSampleAt = previousEnd = reportAt = anomalyAt = 0; stop.Reset();
    calls = {}; callCount = nextCall = keyCalls = keyBlocked = contextClears = 0; lastKeyAt = 0;
    gaps = {}; gapMax = engineMax = admissionMax = moduleMax = stageMax = diagnosticMax = 0; slowest = "-";
}
}
