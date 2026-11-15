// SPDX-License-Identifier: GPL-3.0-or-later
// Host-owned saves. Clients exchange only their account-bound character, never native world files.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace g3t::persist {
void AppendPendingAdmissionAnchors(std::vector<std::array<float, 3>>& out);
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
void SetWorldCleanup(void (*cleanup)());
bool ProcessClientTransition(); // outer module boundary only; true consumes this frame
bool ClientTransitionPending();
// Client: enter the host's current world NOW, through the engine's own LoadGame of the client template slot
// (G3T_client_template, made once from the player's first New Game). Queue only: safe from ENet dispatch or any
// frame; the native work runs at the next outer module boundary (ProcessClientTransition) once no native load/save,
// trade or dialogue is active. Works from the title screen and in-world. The host's world replay and the client's own
// account record are applied afterwards by the existing admission flow.
enum class EnterResult { Queued, AlreadyQueued, Unavailable, NotClient };
EnterResult EnterHostWorld(char const* reason);
bool HostWorldEntryAvailable(); // a way into the host's world without restarting: a valid template, or one the client can make now (file check; not per frame)
void OnWorldActivate();    // from the engine module's callbacks
void OnWorldDeactivate();
bool PrepareShutdown();    // cancel cooperative staging; false while native save/load still owns the stack
namespace SaveExtensions {
using Capture = bool (*)(std::vector<uint8_t>& bytes);
using Restore = bool (*)(uint32_t savedVersion, std::vector<uint8_t> const& bytes);
// Stable unique name/type (>=256), <=1 MiB. Restore version 0/empty clears an absent overlay.
bool Register(char const* name, uint32_t type, uint32_t version, Capture capture, Restore restore);
}
}
