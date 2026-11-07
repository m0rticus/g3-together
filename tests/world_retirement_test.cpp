// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/world_retirement.h"

#include <array>
#include <cassert>
#include <string>
#include <vector>

using namespace g3t::world::retirement;

struct Entity {
    uint32_t databases;
    bool cached = true;
    unsigned releases = 0;
};

struct Cell {
    unsigned gridSize;
    uint32_t slots;
    std::vector<Entity*> entities;
    unsigned disables = 0;
    void DisableDatabaseSlot(uint32_t index)
    {
        ++disables;
        const uint32_t bit = uint32_t(1) << index;
        slots &= ~bit;
        for (size_t i = entities.size(); i > 0; --i) {
            auto* entity = entities[i - 1];
            if (!(entity->databases & bit)) continue;
            entity->databases &= ~bit;
            if (!entity->databases) { entity->cached = false; ++entity->releases; }
            entities.erase(entities.begin() + i - 1);
        }
    }
};

struct Operations {
    bool registered = true, empty = false, owners = true;
    unsigned failAt = 0, calls = 0, destructions = 0;
    std::vector<std::string> events;
    bool Step(char const* name)
    {
        assert(owners);
        events.push_back(name);
        return ++calls != failAt;
    }
    bool CompleteJobs() { return Step("complete"); }
    bool FlushQueues() { return Step("flush"); }
    bool ClearCells()
    {
        if (!Step("clear")) return false;
        empty = true;
        return true;
    }
    bool CellsEmpty() { return Step("empty") && empty; }
    bool Unregister()
    {
        assert(empty);
        if (!Step("unregister")) return false;
        registered = false;
        return true;
    }
    void DestroyOwners()
    {
        assert(empty && !registered && owners);
        events.push_back("destroy");
        owners = false;
        ++destructions;
    }
};

int main()
{
    Entity current{2}, oldGrid{2}, permanentEntity{2}, otherOwner{3};
    Cell currentCell{6000, 2, {&current, &otherOwner}};
    Cell oldCell{4000, 2, {&oldGrid}};
    Cell permanent{0, 2, {&permanentEntity}};
    std::array<Cell*, 5> cells{&currentCell, &oldCell, nullptr, &permanent, &permanent};
    auto at = [&](size_t i) { return cells[i]; };
    assert(DrainSlots(cells.size(), at, &permanent, 1));
    assert(!current.cached && !oldGrid.cached && !permanentEntity.cached);
    assert(current.releases == 1 && oldGrid.releases == 1 && permanentEntity.releases == 1);
    assert(otherOwner.cached && otherOwner.databases == 1 && otherOwner.releases == 0);
    assert(currentCell.entities.empty() && oldCell.entities.empty() && permanent.entities.empty());
    assert(currentCell.disables == 1 && oldCell.disables == 1 && permanent.disables == 1);
    assert(DrainSlots(cells.size(), at, &permanent, 1));
    assert(current.releases == 1 && oldGrid.releases == 1 && permanentEntity.releases == 1);
    const auto before = permanent.disables;
    assert(!DrainSlots(cells.size(), at, &permanent, 32));
    assert(!DrainSlots(cells.size(), at, &permanent, UINT32_MAX));
    assert(permanent.disables == before);

    Entity lastSlot{uint32_t(1) << 31};
    Cell last{6000, uint32_t(1) << 31, {&lastSlot}};
    assert(DrainSlots(1, [&](size_t) { return &last; }, static_cast<Cell*>(nullptr), 31));
    assert(!lastSlot.cached && lastSlot.databases == 0);
    assert(DrainSlots(0, at, static_cast<Cell*>(nullptr), 0));

    const std::vector<std::string> expected{"complete", "flush", "clear", "complete", "flush",
                                          "empty", "unregister", "complete", "flush", "destroy"};
    Operations success;
    assert(Retire(success) && success.events == expected && success.destructions == 1);
    for (unsigned failure = 1; failure < expected.size(); ++failure) {
        Operations operation;
        operation.failAt = failure;
        assert(!Retire(operation) && operation.owners && operation.destructions == 0);
        assert(operation.events.size() == failure);
        operation.failAt = 0;
        operation.calls = 0;
        operation.events.clear();
        assert(Retire(operation) && operation.events == expected && operation.destructions == 1);
    }
    struct Residual : Operations {
        bool CellsEmpty() { events.push_back("empty"); return false; }
    } residual;
    assert(!Retire(residual) && residual.registered && residual.owners);
    assert(residual.events.back() == "empty");

    Gate gate;
    assert(!gate.Paused());
    assert(gate.Begin() && gate.Paused());
    assert(!gate.Begin() && gate.Paused());
    gate.Activate();
    assert(gate.Paused());
    gate.Finish();
    assert(gate.Paused());
    assert(gate.Begin());
    gate.Finish();
    gate.Activate();
    assert(!gate.Paused());
}
