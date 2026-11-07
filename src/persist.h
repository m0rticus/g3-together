// SPDX-License-Identifier: GPL-3.0-or-later
// Host-owned saves. Clients exchange only their account-bound character, never native world files.
#pragma once

#include <cstdint>
#include <vector>

namespace g3t::persist {
void Install();          // once the game DLLs are loaded; refuses (and logs) on any other build
void* SaveTarget();      // the game's SaveGame entry (checked before hooking), nullptr if not hooked
void* LoadTarget();
bool CheckpointBusy();   // player exchanges wait while the host collects characters and saves
// Restore the account-exact character before admission; clients never load the host's native world.
void Tick();
// Around the engine module's Process. BeginFrame first: false, and nothing of the mod runs in this frame, while the
// game's own load runs (its Session::Start runs frames of its own, C 22.10.); true otherwise, and EndFrame follows.
bool BeginFrame();
void EndFrame();
bool NativeLoading();      // no native entity inspection until the synchronous loader returns
bool HeroPending();        // world/character restoration or fresh local New Game is still required
void OnWorldActivate();    // from the engine module's callbacks
void OnWorldDeactivate();
using LogFn = void (*)(char const*);
void Off(LogFn log);     // QA negative control: write no sidecar
// Retired compatibility commands; they only explain retirement, never alter saving/admission.
void CampaignOff(LogFn log);
void WorldSyncOff(LogFn log);
void BadTransfer(LogFn log);
void GenerationOff(LogFn log);
void Pause(char const* argument, LogFn log);
void WorldGateLate(LogFn log);
void RecordWaitOff(LogFn log);
void Waiting(LogFn log);       // QA (stage W7): RECORDWAIT line, whether the player waits for its own hero record
void LoadTicksOff(char const* argument, LogFn log);  // QA stage W6: negative control; hold/release/welcome are deterministic timing probes
void WorldReport(LogFn log);   // QA: WORLD line
void Report(LogFn log);  // QA: PERSIST line
bool Switch(char const* action, LogFn log);  // QA negative controls of the play-evening fixes (27.10.): true if it was one
namespace SaveExtensions {
using Capture = bool (*)(std::vector<uint8_t>& bytes);
using Restore = bool (*)(uint32_t savedVersion, std::vector<uint8_t> const& bytes);
// Stable unique name/type (>=256), <=1 MiB. Restore version 0/empty clears an absent overlay.
bool Register(char const* name, uint32_t type, uint32_t version, Capture capture, Restore restore);
}
}
