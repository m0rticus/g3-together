// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace g3t::crashlog {
void Stage(char const* name);
void StartWatchdog();
void CompletePreShutdown(bool safeToExit);
}
