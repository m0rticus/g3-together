// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldentry_diag.h"

#include "ai.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "symbols.h"

#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>

namespace g3t::crashlog {
namespace {
worldentry::Progress entryOperation, entryGate;
std::atomic<uint32_t> entryEpoch{0}, entryWelcome{0};
worldentry::Reports entryReports;
std::atomic<DWORD> entryThread{0};

struct CacheSnapshot {
    std::atomic<uint32_t> count{0};
    std::atomic<void const*> first{nullptr};
};
CacheSnapshot cacheSnapshots[static_cast<size_t>(CacheKind::Count)];
char const* const cacheNames[] = {"cameras", "prefetchers", "private_entities", "spheres", "anchors",
                                  "native_range", "puppets", "disabled", "summon_records", "npc_lease_records"};
static_assert(std::size(cacheNames) == std::size(cacheSnapshots), "cache inventory names");
}

void WorldEntryWait(char const* step)
{
    if (GetConfig().host || ai::ShuttingDown()) return;
    entryEpoch.store(net::WorldEpoch());
    entryWelcome.store(net::WelcomeSerial());
    const auto previous = entryGate.Set(step, GetTickCount());
    if (!step && previous.name)
        Logf("world-entry: complete epoch=%08x welcome=%u", entryEpoch.load(), entryWelcome.load());
}

WorldEntryScope::WorldEntryScope(char const* operation)
{
    active = !GetConfig().host && !ai::ShuttingDown() && entryGate.Read().name;
    if (active) {
        entryThread.store(GetCurrentThreadId());
        previous = entryOperation.Set(operation, GetTickCount());
    }
}

WorldEntryScope::~WorldEntryScope()
{
    Complete();
}

void WorldEntryScope::Complete()
{
    if (!active) return;
    entryOperation.Restore(previous);
    active = false;
}

bool ReadWorldEntry(WorldEntryState& out)
{
    if (!entryGate.Read(out.gate) || !out.gate.name || !entryOperation.Read(out.operation)) return false;
    out.epoch = entryEpoch.load();
    out.welcome = entryWelcome.load();
    out.threadId = entryThread.load();
    return true;
}

bool WorldEntryReportDue(uint32_t now, worldentry::Step const& operation, worldentry::Step const& gate)
{
    return entryReports.Due(now, operation, gate);
}

void CacheObjects(CacheKind kind, uint32_t count, void const* first)
{
    auto& snapshot = cacheSnapshots[static_cast<size_t>(kind)];
    snapshot.first.store(first);
    snapshot.count.store(count);
}

void LogCacheInventory(char const* phase)
{
    char line[800];
    int length = Format(line, "world-cache: inventory %s", phase);
    for (size_t i = 0; i < std::size(cacheSnapshots) && length > 0; ++i) {
        const int written = Format(line + length, sizeof(line) - length, " %s=%u/%p", cacheNames[i],
                                   cacheSnapshots[i].count.load(), cacheSnapshots[i].first.load());
        if (written < 0 || static_cast<size_t>(length + written) >= sizeof(line)) {
            Log("FAIL world-cache: inventory diagnostic exceeded its buffer");
            return;
        }
        length += written;
    }
    Log(line);
}

void NativeStack(char const* reason)
{
    void* frames[24] = {};
    const USHORT count = CaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    Logf("CRASH native-stack reason=%s pid=%lu thread=%lu frames=%u", reason,
         GetCurrentProcessId(), GetCurrentThreadId(), static_cast<unsigned>(count));
    for (USHORT i = 0; i < count; ++i) {
        char where[MAX_PATH + 24];
        if (!symbols::DescribeImageAddress(reinterpret_cast<uintptr_t>(frames[i]), where, sizeof(where)))
            strcpy_s(where, "(module unavailable)");
        Logf("CRASH native-stack frame=%u address=%p %s", static_cast<unsigned>(i), frames[i], where);
    }
    // x86 engine functions omit frame pointers. Keep bounded raw candidates separately, never label them unwound frames.
    auto* stack = static_cast<uintptr_t const*>(_AddressOfReturnAddress());
    auto const* tib = reinterpret_cast<NT_TIB const*>(NtCurrentTeb());
    const uintptr_t begin = reinterpret_cast<uintptr_t>(stack), top = reinterpret_cast<uintptr_t>(tib->StackBase);
    if (begin < reinterpret_cast<uintptr_t>(tib->StackLimit) || begin >= top) {
        Log("FAIL CRASH native-stack: current-thread stack bounds unavailable");
        return;
    }
    const size_t words = std::min<size_t>((top - begin) / sizeof(uintptr_t), 1024);
    uintptr_t candidates[32] = {};
    unsigned retained = 0;
    for (size_t i = 0; i < words && retained < std::size(candidates); ++i) {
        if (!stack[i] || std::find(candidates, candidates + retained, stack[i]) != candidates + retained) continue;
        char where[MAX_PATH + 24];
        if (!symbols::DescribeImageAddress(stack[i], where, sizeof(where))) continue;
        candidates[retained++] = stack[i];
        Logf("CRASH native-stack candidate_word=%u address=%p %s", static_cast<unsigned>(i),
             reinterpret_cast<void*>(stack[i]), where);
    }
}
}
