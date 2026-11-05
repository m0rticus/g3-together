// SPDX-License-Identifier: GPL-3.0-or-later
// Personal chest/corpse loot and NPC merchant inventories. Native local generation/transfers are retained;
// remote inventory messages never overwrite loot or roll back the hero. Direct player trades remain in trade.cpp.
#pragma once

class eCEntity;

namespace g3t::containers {
bool IsPersonalInventory(eCEntity* entity);  // world inventory, not the hero, an item or another player's puppet
void Start();
void Tick();
void OnWorldChange();  // forget every container state of the world before (load, world change)
using LogFn = void (*)(char const*);
void Contents(char const* guid, LogFn log);  // QA: CONTENTS line for a container
void Open(char const* guid, LogFn log);      // QA: open a container the player's way (PlayerOpenLoot)
void Take(char const* guid, LogFn log);      // QA: move the first stack of a container to the hero
void Report(LogFn log);                      // QA: CONTAINERSYNC counters
void Disable(char const* what, LogFn log);    // QA negative control ("trader": only the host's trader filling; "hands": a corpse's hand keeps its prop; "interact": a dug treasure's loot is not this player's open container)
void Silent(char const* guid, LogFn log);    // QA (host): drop a stack without telling the clients
void TradeOpen(char const* guid, LogFn log);  // QA: open trade with an NPC (PlayerOpenTrade)
void TradeBegin(char const* argument, LogFn log);  // QA: the trade screen as DoTrade opens it (BeginTrade)
void TradeBuy(char const* argument, LogFn log);  // QA: buy one piece ("GUID" or "GUID_Template[-q<quality>]") and pay
void TradeSell(char const* argument, LogFn log); // QA: sell one piece of the hero's ("GUID_Template[-q<quality>]"), be paid
void Goods(char const* guid, LogFn log);      // QA: GOODS line for any inventory
void Stock(char const* argument, LogFn log);  // QA (host): add one item to an NPC ("GUID_Template")
void InvState(char const* guid, LogFn log);   // QA: INVSTATE line (rolled from treasure sets yet?, sets, items)
void Stacks(char const* guid, LogFn log);     // QA: STACK lines, every stack with its slot and whether it counts
void HoldItem(char const* argument, LogFn log); // QA (host): "GUID_Template" into an NPC's right hand
void Chests(LogFn log);                       // QA: CHEST lines within 40 m, nearest first
void QuickLoot(LogFn log);                    // QA: queue the game's quick loot action (as Use2 in PS_Normal does)
void QuickLootActions(LogFn log);             // QA: QUICKACTIONS line (entries in the game's action list)
bool Switch(char const* action, LogFn log);  // QA negative controls of the play-evening fixes (27.10.): true if it was one
}
