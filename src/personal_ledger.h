// SPDX-License-Identifier: GPL-3.0-or-later
// Personal world-inventory ledger, observer pool, RAII mutation guard, and rollback-safe snapshot restore.
#pragma once

#include "personal_store.h"

#include <Game.h>
#include <Script.h>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace g3t::containers::ledger {

struct ObserverState;

bool HasInventory(eCEntity* entity);
bool IsContainer(Entity& entity);
bool IsChest(Entity& entity);
bool IsTrader(Entity& entity);
bool Shared(Entity& entity);
bool Corpse(Entity& entity);
bool PersonalKind(Entity& entity, saving::personal::Kind& kind);
std::string EntityKey(eCEntity* entity);
std::string NameOf(Entity& entity);

bool PersonalAdmitted();
bool IsPending(std::string const& key);
bool IsRestored(std::string const& key);
void MarkTradeOpened(std::string const& key);
void TrackTradeClose();

bool PersonalTeardown(Entity& entity);
bool EnsurePersonal(Entity& entity);
bool RememberPersonal(Entity& entity);
bool TrackPersonalFocus();
bool PersonalPass(bool capture, std::set<std::string>* attempted = nullptr);
bool FlushPersonalObservers();

void Reset();
bool Capture(std::vector<uint8_t>& out, bool (*installAccessGuards)());
bool Restore(std::vector<uint8_t> const& bytes, bool (*installAccessGuards)());

class PersonalMutation {
public:
    PersonalMutation() = default;
    PersonalMutation(PersonalMutation const&) = delete;
    PersonalMutation& operator=(PersonalMutation const&) = delete;
    bool Enter(gCInventory_PS* inventory);
    bool StillAvailable(gCInventory_PS* inventory) const;
    void Finish();
    ~PersonalMutation() { Finish(); }

private:
    std::vector<ObserverState*> held_;
};

} // namespace g3t::containers::ledger
