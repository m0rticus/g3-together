// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net.h"
#include "trade.h"
#include "trade_policy.h"

#include <Game.h>
#include <Script.h>
#include <cstdint>
#include <map>
#include <string>

namespace g3t::trade::detail {

using Content = std::map<std::string, int>;

struct Wares {
    bool known = false;
    uint64_t epoch = 0;
    uint32_t serial = 0;
    int32_t gold = 0;
    Content items;
};

struct SyncSnapshot {
    unsigned goodsSent = 0;
    uint32_t serial = 0;
    size_t ownItems = 0;
    int32_t ownGold = -1;
    unsigned goodsReceived = 0;
    int partner = 0;
    unsigned fills = 0;
    unsigned swaps = 0;
    bool nativeHooked = false;
    bool costHooked = false;
    bool windowOpen = false;
    unsigned heroReleases = 0;
};

void ReadInventory(Entity& self, Content& items, int32_t& gold);
SyncSnapshot GetSyncSnapshot();
bool CopyKnownGoods(int player, Content& itemsOut, int32_t& goldOut, uint32_t& serialOut);
std::string ItemNameForKey(std::string const& key);
void SetFillOff(bool off);

} // namespace g3t::trade::detail
