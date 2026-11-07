// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace g3t::world::retirement {
class Gate {
    std::atomic<bool> paused{false}, cleaning{false};
  public:
    bool Begin()
    {
        paused.store(true);
        return !cleaning.exchange(true);
    }
    void Finish() { cleaning.store(false); }
    void Activate() { if (!cleaning.load()) paused.store(false); }
    bool Paused() const { return paused.load(); }
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
