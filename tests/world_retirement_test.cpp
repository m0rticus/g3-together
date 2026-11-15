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

    {
        Gate stopped;
        assert(stopped.Begin(true));
        stopped.Finish();
        assert(stopped.Paused() && stopped.Resumable());
        assert(!stopped.ResumeAfterStop(false));
        assert(stopped.Paused() && stopped.Resumable());
        assert(stopped.ResumeAfterStop(true));
        assert(!stopped.Paused() && !stopped.Resumable());
        assert(!stopped.ResumeAfterStop(true));
        assert(!stopped.Paused() && !stopped.Resumable());
    }

    {
        Gate callback;
        assert(callback.Begin());
        callback.Finish();
        assert(callback.Paused() && !callback.Resumable());
        assert(!callback.ResumeAfterStop(true));
        assert(callback.Paused() && !callback.Resumable());
        callback.Activate();
        assert(!callback.Paused() && !callback.Resumable());
    }

    {
        Gate cleaning;
        assert(cleaning.Begin(true));
        assert(cleaning.Paused() && cleaning.Resumable());
        assert(!cleaning.ResumeAfterStop(true));
        assert(cleaning.Paused() && cleaning.Resumable());
        assert(!cleaning.Begin(true));
        assert(cleaning.Resumable());
        cleaning.Finish();
        assert(cleaning.ResumeAfterStop(true));

        assert(cleaning.Begin(false));
        assert(cleaning.Paused() && !cleaning.Resumable());
        assert(!cleaning.Begin(true));
        assert(cleaning.Paused() && !cleaning.Resumable());
        cleaning.Finish();
        assert(!cleaning.ResumeAfterStop(true));
        assert(cleaning.Paused() && !cleaning.Resumable());
        cleaning.Activate();
        assert(!cleaning.Paused());
    }

    {
        Gate newGame;
        assert(newGame.Begin(false));
        newGame.Finish();
        newGame.Activate();
        assert(!newGame.Paused() && !newGame.Resumable());
        assert(newGame.Begin(true));
        newGame.Finish();
        assert(newGame.Paused() && newGame.Resumable());
        assert(newGame.ResumeAfterStop(true));
        assert(!newGame.Paused() && !newGame.Resumable());
    }

    {
        Gate callbackAfterStop;
        assert(callbackAfterStop.Begin(true));
        callbackAfterStop.Finish();
        assert(callbackAfterStop.Paused() && callbackAfterStop.Resumable());
        assert(callbackAfterStop.Begin(false));
        callbackAfterStop.Finish();
        assert(callbackAfterStop.Paused() && !callbackAfterStop.Resumable());
        assert(!callbackAfterStop.ResumeAfterStop(true));
        callbackAfterStop.Activate();
        assert(!callbackAfterStop.Paused() && !callbackAfterStop.Resumable());
    }

    {
        Gate activate;
        assert(activate.Begin(true));
        activate.Finish();
        assert(activate.Resumable());
        activate.Activate();
        assert(!activate.Paused() && !activate.Resumable());
        assert(!activate.ResumeAfterStop(true));
    }

    {
        // Build 70 regression: remote camera registered database=1 globally; private prefetcher has 0 entities while
        // mainPrefetcher cached remoteOnly (databases=0x2) and sharedHeroEntity (databases=0x3 = slot 0 | slot 1).
        struct Prefetcher {
        std::vector<Cell*> valid;
        Cell permanent{0, 0, {}};
        unsigned cleared = 0;
        void ClearCells(unsigned&) { ++cleared; }
        };
        Prefetcher privatePrefetcher, mainPrefetcher;
        Entity remoteOnly{0x2}, sharedHeroEntity{0x3};
        Cell mainCell{6000, 0x3, {&remoteOnly, &sharedHeroEntity}};
        mainPrefetcher.valid.push_back(&mainCell);
        unsigned db = 1;
        auto drainPrefetcher = [](Prefetcher& p, unsigned& database, uint32_t slot) {
        if (!DrainSlots(p.valid.size(), [&](size_t i) { return p.valid[i]; }, &p.permanent, slot)) return false;
        p.ClearCells(database);
        return true;
        };
        auto scanCell = [](Cell const& cell, uint32_t bit) {
        size_t n = 0;
        for (auto const* entity : cell.entities)
            if (entity && (entity->databases & bit)) ++n;
        return n;
        };
        assert(CountSlotEntities(privatePrefetcher.valid.size(), [&](size_t i) { return privatePrefetcher.valid[i]; },
                             &privatePrefetcher.permanent, 1, scanCell) == 0);
        assert(CountSlotEntities(mainPrefetcher.valid.size(), [&](size_t i) { return mainPrefetcher.valid[i]; },
                             &mainPrefetcher.permanent, 1, scanCell) == 2);

        assert(!DrainDualPrefetchers(&privatePrefetcher, mainPrefetcher, db, 32, drainPrefetcher));
        assert(DrainDualPrefetchers(&privatePrefetcher, mainPrefetcher, db, 0xffffffffu, drainPrefetcher));
        assert(privatePrefetcher.cleared == 0 && mainPrefetcher.cleared == 0);

        assert(DrainDualPrefetchers(&privatePrefetcher, mainPrefetcher, db, 1, drainPrefetcher));
        assert(privatePrefetcher.cleared == 1 && mainPrefetcher.cleared == 1);
        assert(!remoteOnly.cached && remoteOnly.databases == 0 && remoteOnly.releases == 1);
        assert(sharedHeroEntity.cached && sharedHeroEntity.databases == 0x1 && sharedHeroEntity.releases == 0);
        assert(CountSlotEntities(mainPrefetcher.valid.size(), [&](size_t i) { return mainPrefetcher.valid[i]; },
                             &mainPrefetcher.permanent, 1, scanCell) == 0);

        // Multi-region regression: retiring slot 1 when sibling remote region (slot 2) also cached an entity with
        // databases = 0x6 (slot 1 | slot 2) in siblingPrefetcher must clear slot 1 from siblingPrefetcher too.
        Prefetcher region1Prefetcher, siblingPrefetcher, multiMainPrefetcher;
        Entity region1Only{0x2}, sharedBetweenRemotes{0x6};
        Cell siblingCell{6000, 0x6, {&region1Only, &sharedBetweenRemotes}};
        siblingPrefetcher.valid.push_back(&siblingCell);
        std::array<Prefetcher*, 2> siblings{&siblingPrefetcher, nullptr};
        assert(CountSlotEntities(siblingPrefetcher.valid.size(), [&](size_t i) { return siblingPrefetcher.valid[i]; },
                                 &siblingPrefetcher.permanent, 1, scanCell) == 2);
        assert(!DrainPrefetcherGroup(&region1Prefetcher, siblings.data(), siblings.size(), multiMainPrefetcher, db, 32,
                                     drainPrefetcher));
        assert(DrainPrefetcherGroup(&region1Prefetcher, siblings.data(), siblings.size(), multiMainPrefetcher, db, 1,
                                    drainPrefetcher));
        assert(region1Prefetcher.cleared == 1 && siblingPrefetcher.cleared == 1 && multiMainPrefetcher.cleared == 1);
        assert(!region1Only.cached && region1Only.databases == 0 && region1Only.releases == 1);
        assert(sharedBetweenRemotes.cached && sharedBetweenRemotes.databases == 0x4 && sharedBetweenRemotes.releases == 0);
        assert(CountSlotEntities(siblingPrefetcher.valid.size(), [&](size_t i) { return siblingPrefetcher.valid[i]; },
                                 &siblingPrefetcher.permanent, 1, scanCell) == 0);
    }
}
