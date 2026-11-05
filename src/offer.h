// SPDX-License-Identifier: GPL-3.0-or-later
// Reliable automatic player exchanges. Inventory owners validate and apply once; retries only repeat the outcome.
#pragma once

#include "net.h"

namespace g3t::offer {
using ResultFn = bool (*)(net::OfferNote const&, bool accepted);
void Start();
void Tick();
void OnWorldChange();
void SetResultHandler(ResultFn handler);
bool Send(net::OfferNote note);
bool Reserve(net::OfferNote const& note);
bool Deliver(net::OfferNote const& note, bool accepted);
bool Idle();
using LogFn = void (*)(char const*);
void Report(LogFn log);
void AddCommand(char const* argument, LogFn log);
void CountCommand(char const* name, LogFn log);
}
