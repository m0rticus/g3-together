// SPDX-License-Identifier: GPL-3.0-or-later
// Free player exchanges through Gothic 3's NPC trade screen. Native transaction selections, no approval keys.
#pragma once

#include <cstdint>
class eCEntity;

namespace g3t::trade {
void Start();
void Tick();
void OnWorldChange();
bool OnOpen(eCEntity* partner);
bool Available();
bool ReadyForSave();
uint64_t Epoch();
uint32_t Publish();
void RestoreGold(eCEntity* puppet, int player);
using LogFn = void (*)(char const*);
void Report(LogFn log);
void GoodsReport(char const* argument, LogFn log);
void FillOff(LogFn log);
void Price(char const* argument, LogFn log);
void GiveQuality(char const* argument, LogFn log);
void QualityCount(char const* name, LogFn log);
void CloseCommand(LogFn log);
void GiftCommand(char const* argument, LogFn log);
bool Exchange(eCEntity* partner, char const* item, int amount, bool take);
}
