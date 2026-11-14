// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace g3t::world::retirement {
class Gate {
    std::atomic<bool> paused{false}, cleaning{false}, resumable{false};
  public:
    // bySession: the pause comes from the native gCSession::Stop. A world (de)activation callback is always followed by
    // Activate; a Stop is not (HUD.NewGame stops the session after the activation callback and starts it again without a
    // second one), so only that kind of pause may be lifted again once the session runs.
    bool Begin(bool bySession = false)
    {
        paused.store(true);
        const bool entered = !cleaning.exchange(true);
        if (entered) resumable.store(bySession);
        return entered;
    }
    void Finish() { cleaning.store(false); }
    void Activate()
    {
        if (cleaning.load()) return;
        paused.store(false);
        resumable.store(false);
    }
    bool ResumeAfterStop(bool sessionRunning)
    {
        if (!sessionRunning || cleaning.load() || !paused.load() || !resumable.load()) return false;
        resumable.store(false);
        paused.store(false);
        return true;
    }
    bool Paused() const { return paused.load(); }
    bool Resumable() const { return resumable.load(); }
};

template<class Cell, class CellAt>
bool DrainSlots(std::size_t count, CellAt cellAt, Cell* permanent, uint32_t index)
{
    if (index >= 32) return false;
    for (std::size_t i = 0; i < count; ++i)
        if (auto* cell = cellAt(i); cell && cell != permanent) cell->DisableDatabaseSlot(index);
    if (permanent) permanent->DisableDatabaseSlot(index);
    return true;
}

template<class Cell, class CellAt, class ScanCell>
std::size_t CountSlotEntities(std::size_t count, CellAt cellAt, Cell* permanent, uint32_t index, ScanCell scanCell)
{
    if (index >= 32) return 0;
    const uint32_t bit = uint32_t(1) << index;
    std::size_t matching = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (auto* cell = cellAt(i); cell && cell != permanent) matching += scanCell(*cell, bit);
    if (permanent) matching += scanCell(*permanent, bit);
    return matching;
}

template<class Prefetcher, class Database, class DrainPrefetcher>
bool DrainDualPrefetchers(Prefetcher* privatePrefetcher, Prefetcher& mainPrefetcher, Database& database, uint32_t index,
                          DrainPrefetcher drainPrefetcher)
{
    if (index == 0xffffffffu) return true;
    if (index >= 32) return false;
    if (privatePrefetcher && !drainPrefetcher(*privatePrefetcher, database, index)) return false;
    return drainPrefetcher(mainPrefetcher, database, index);
}

template<class Operations>
bool Retire(Operations& operations)
{
    if (!operations.CompleteJobs() || !operations.FlushQueues()) return false;
    if (!operations.ClearCells()) return false;
    if (!operations.CompleteJobs() || !operations.FlushQueues() || !operations.CellsEmpty()) return false;
    if (!operations.Unregister()) return false;
    if (!operations.CompleteJobs() || !operations.FlushQueues()) return false;
    operations.DestroyOwners();
    return true;
}
}
